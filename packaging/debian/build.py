#!/usr/bin/env python3
"""Build the mAgicVISION USB DKMS Debian package from public host sources."""
import argparse
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
TEMPLATES = Path(__file__).resolve().parent
STAGE = Path('/private/tmp' if sys.platform == 'darwin' else '/tmp') / ('mb86H-deb-build-' + str(os.getuid()))
BUDGET = 16 * 1024 * 1024
OWNER = {'task': 'mb86H-deb-build', 'uid': os.getuid(), 'source': str(ROOT)}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sources(revision):
    """Only Kbuild's USB translation units and public/local headers are shipped."""
    def read(name):
        return subprocess.check_output(['git', 'show', revision + ':' + name], cwd=ROOT)
    tracked = set(subprocess.check_output(
        ['git', 'ls-tree', '-r', '--name-only', revision], cwd=ROOT, text=True).splitlines())
    kbuild = read('Kbuild').decode()
    composition = kbuild.split('magicvision_ccid-y :=', 1)[1].split('\n\n', 1)[0]
    composition += ' ' + kbuild.split('magicvision_usb-y :=', 1)[1].split('\n\n', 1)[0]
    paths = {'Kbuild', 'linux/ccid_metadata.py', 'linux/60-mb86H.rules'}
    for obj in composition.replace('\\\n', ' ').split():
        base = obj[:-2]
        found = [base + ext for ext in ('.c', '.S') if base + ext in tracked]
        if len(found) != 1:
            raise ValueError('cannot resolve Kbuild object ' + obj)
        paths.add(found[0])
    for directory in ('include', 'linux', 'core', 'usb', 'device'):
        paths.update(p for p in tracked if p.startswith(directory + '/') and p.endswith('.h'))
    return {name: read(name) for name in sorted(paths)}


def size(path):
    return sum(p.stat().st_size for p in path.rglob('*') if p.is_file())


def clean_stage():
    if not STAGE.exists() and not STAGE.is_symlink():
        return 0
    marker = STAGE / '.owner.json'
    if STAGE.is_symlink() or STAGE.stat().st_uid != os.getuid() or \
            not marker.is_file() or json.loads(marker.read_text()) != OWNER:
        raise ValueError('refusing foreign staging path: ' + str(STAGE))
    removed = size(STAGE)
    shutil.rmtree(STAGE)
    if STAGE.exists():
        raise ValueError('staging cleanup failed')
    return removed


def put(base, name, data, mode=0o644):
    target = base / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data.encode() if isinstance(data, str) else data)
    target.chmod(mode)


def build(args):
    stale_removed = clean_stage()
    revision = subprocess.check_output(['git', 'rev-parse', '--verify', args.revision + '^{commit}'],
                                       cwd=ROOT, text=True).strip()
    payload = sources(revision)
    templates = {p.name: p.read_bytes() for p in sorted(TEMPLATES.iterdir()) if p.is_file()}
    inputs = {**payload, **{'packaging/debian/' + k: v for k, v in templates.items()}}
    digest = sha(b''.join(name.encode() + b'\0' + data for name, data in sorted(inputs.items())))
    version = args.version or '0.1.0+src.' + digest[:12]
    if not re.fullmatch(r'[0-9][a-zA-Z0-9.+~]*', version):
        raise ValueError('invalid upstream Debian version')
    maintainer = args.maintainer
    if not maintainer:
        name = subprocess.check_output(['git', 'config', 'user.name'], cwd=ROOT, text=True).strip()
        email = subprocess.check_output(['git', 'config', 'user.email'], cwd=ROOT, text=True).strip()
        maintainer = name + ' <' + email + '>'
    if '\n' in maintainer or '<' not in maintainer or '>' not in maintainer:
        raise ValueError('supply --maintainer "Name <email>"')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {'version': version, 'host': sys.platform, 'cwd': str(ROOT), 'source_revision': revision,
              'started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'inputs': {k: {'sha256': sha(v), 'bytes': len(v)} for k, v in inputs.items()},
              'commands': [], 'packages': []}
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH') or subprocess.check_output(
        ['git', 'show', '-s', '--format=%ct', revision], cwd=ROOT, text=True).strip())
    env = dict(os.environ, SOURCE_DATE_EPOCH=str(epoch), COPYFILE_DISABLE='1')

    def run(command, binary=False):
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True)
        report['commands'].append({'argv': command, 'cwd': str(ROOT), 'exit_status': result.returncode,
                                   'stdout': '<tar stream>' if binary else result.stdout.decode(errors='replace'),
                                   'stderr': result.stderr.decode(errors='replace'),
                                   'time': datetime.datetime.now(datetime.timezone.utc).isoformat()})
        if result.returncode:
            raise RuntimeError('command failed: ' + repr(command) + '\n' + result.stderr.decode(errors='replace'))
        return result.stdout

    def control(base, package, description, depends=None):
        fields = f'Package: {package}\nVersion: {version}\nArchitecture: all\nSection: kernel\nPriority: optional\nMaintainer: {maintainer}\n'
        if depends:
            fields += 'Depends: ' + depends + '\n'
            fields += 'Recommends: linux-headers-generic | linux-headers-amd64 | linux-headers-arm64\n'
        installed = sum(p.stat().st_size for p in base.rglob('*') if p.is_file())
        fields += f'Installed-Size: {(installed + 1023) // 1024}\nDescription: {description}\n'
        put(base, 'DEBIAN/control', fields)
        md5s = ''.join(hashlib.md5(p.read_bytes()).hexdigest() + '  ' + str(p.relative_to(base)) + '\n'
                       for p in sorted(base.rglob('*')) if p.is_file() and 'DEBIAN' not in p.relative_to(base).parts)
        put(base, 'DEBIAN/md5sums', md5s)

    def archive(base, package):
        if size(STAGE) > BUDGET:
            raise ValueError('staging exceeds 16 MiB budget')
        for p in sorted(base.rglob('*'), reverse=True) + [base]:
            os.utime(p, (epoch, epoch))
            if p.is_dir():
                p.chmod(0o755)
        target = output / (package + '_' + version + '_all.deb')
        run(['dpkg-deb', '--root-owner-group', '-Zgzip', '-z9', '--uniform-compression', '--build', str(base), str(target)])
        info = run(['dpkg-deb', '--info', str(target)]).decode()
        listing = run(['dpkg-deb', '--contents', str(target)]).decode()
        put(output, target.name + '.inspection.txt', info + '\n' + listing)
        data = run(['dpkg-deb', '--fsys-tarfile', str(target)], binary=True)
        members = []
        with tarfile.open(fileobj=io.BytesIO(data)) as tar:
            for entry in tar:
                if entry.uid or entry.gid or entry.issym() or entry.islnk():
                    raise ValueError('unexpected payload ownership or link: ' + entry.name)
                if entry.isfile():
                    contents = tar.extractfile(entry).read()
                    original = base / entry.name.removeprefix('./')
                    if sha(contents) != sha(original.read_bytes()):
                        raise ValueError('payload changed: ' + entry.name)
                    members.append({'path': entry.name, 'sha256': sha(contents), 'mode': oct(entry.mode)})
        report['packages'].append({'path': str(target), 'sha256': sha(target.read_bytes()),
                                   'bytes': target.stat().st_size, 'payload': members})

    report['stale_staging_removed_bytes'] = stale_removed
    STAGE.mkdir(mode=0o700)
    try:
        put(STAGE, '.owner.json', json.dumps(OWNER), 0o600)
        if sum(map(len, inputs.values())) > BUDGET:
            raise ValueError('inputs exceed 16 MiB budget')
        dkms = STAGE / 'dkms'
        source = f'usr/src/magicvision-{version}/'
        for name, contents in payload.items():
            if name not in ('linux/ccid_metadata.py', 'linux/60-mb86H.rules'):
                put(dkms, source + name, contents)
        put(dkms, source + 'dkms.conf', templates['dkms.conf'].decode().replace('@VERSION@', version))
        put(dkms, 'usr/lib/magicvision/ccid-metadata', payload['linux/ccid_metadata.py'], 0o755)
        put(dkms, 'usr/lib/udev/rules.d/60-magicvision.rules', payload['linux/60-mb86H.rules'])
        put(dkms, 'etc/modprobe.d/magicvision.conf', 'options dummy_hcd num=2\n')
        put(dkms, 'usr/lib/modules-load.d/magicvision.conf', 'magicvision_ccid\nmagicvision_usb\n')
        put(dkms, 'DEBIAN/conffiles', '/etc/modprobe.d/magicvision.conf\n')
        put(dkms, 'DEBIAN/triggers', 'interest-noawait /usr/lib/pcsc/drivers\n')
        for name in ('postinst', 'prerm', 'postrm'):
            put(dkms, 'DEBIAN/' + name, templates[name].decode().replace('@VERSION@', version), 0o755)
            run(['sh', '-n', str(dkms / 'DEBIAN' / name)])
        run(['bash', '-n', str(dkms / source / 'dkms.conf')])
        run(['bash', '-eu', '-c',
             'kernel_source_dir=/lib/modules/example/build; dkms_tree=/var/lib/dkms; '
             'source "$1"; test "$PACKAGE_NAME" = magicvision; '
             'test "$BUILT_MODULE_NAME" = magicvision_usb; test "${BUILT_MODULE_NAME[1]}" = magicvision_ccid; '
             'test "${MAKE[0]}" = "make -C /lib/modules/example/build '
             'M=/var/lib/dkms/magicvision/$PACKAGE_VERSION/build '
             'USB_BUNDLE=/usr/lib/firmware/magicvision/firmware.bin modules"',
             'dkms-conf-check', str(dkms / source / 'dkms.conf')])
        for module in ('magicvision_usb', 'magicvision_ccid'):
            if ('obj-m += ' + module + '.o').encode() not in payload['Kbuild']:
                raise ValueError('source revision does not build ' + module)
        control(dkms, 'magicvision-usb-dkms', 'mb86H Series USB TV tuner and smart card drivers (DKMS)\n Linux DVB drivers for mb86H Series ISDB-T and ISDB-S receivers, with\n USB CCID smart card reader support. Builds magicvision_usb and\n magicvision_ccid for the installed kernel and integrates with udev and PC/SC.',
                f'dkms (>= 2.8.7), kmod, udev, python3, libccid, pcscd, magicvision-firmware (= {version})')
        archive(dkms, 'magicvision-usb-dkms')
        report['result'] = 'passed'
    except BaseException as error:
        report['result'] = 'failed'
        report['error'] = str(error)
        raise
    finally:
        report['staging_removed_bytes'] = clean_stage()
        report['staging_absent'] = not STAGE.exists()
        put(output, 'build-' + version + '.json', json.dumps(report, indent=2) + '\n')
    for package in report['packages']:
        print(package['path'] + '\n  sha256: ' + package['sha256'])
    print(f'Staging cleaned: {STAGE} ({report["staging_removed_bytes"]} bytes)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/debian')
    parser.add_argument('--version', required=True, help='matching external firmware package version')
    parser.add_argument('--revision', default='HEAD', help='committed public source revision')
    parser.add_argument('--maintainer')
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    try:
        build(args)
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
