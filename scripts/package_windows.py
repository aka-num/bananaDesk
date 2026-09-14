#!/usr/bin/env python3
"""Assemble a Windows x64 portable directory from a locked MSYS2 mingw64 SDK.

No package scripts are executed. objdump inspects PE files without loading them.
The SDK package archives are used only to identify file ownership and provenance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile


SYSTEM_DLLS = set('''advapi32.dll avrt.dll bcrypt.dll bcryptprimitives.dll
comctl32.dll comdlg32.dll crypt32.dll cryptbase.dll d2d1.dll d3d9.dll d3d11.dll
d3d12.dll d3dcompiler_47.dll dbghelp.dll dcomp.dll dnsapi.dll dwmapi.dll dwrite.dll
dxgi.dll dxva2.dll gdi32.dll gdiplus.dll imm32.dll iphlpapi.dll kernel32.dll
mf.dll mfcore.dll mfplat.dll mfreadwrite.dll mpr.dll msimg32.dll msvcrt.dll
ncrypt.dll netapi32.dll normaliz.dll ntdll.dll ole32.dll oleaut32.dll opengl32.dll
powrprof.dll propsys.dll psapi.dll rpcrt4.dll secur32.dll setupapi.dll shell32.dll
shlwapi.dll user32.dll userenv.dll usp10.dll uxtheme.dll version.dll winhttp.dll
wininet.dll winmm.dll winspool.drv wintrust.dll ws2_32.dll wtsapi32.dll'''.split())


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def inspect_pe(path, objdump):
    result = subprocess.run([objdump, '-p', str(path)], check=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            env={**os.environ, 'LC_ALL': 'C'})
    if 'file format pei-x86-64' not in result.stdout:
        raise RuntimeError(f'Not a Windows x64 PE file: {path}')
    return re.findall(r'DLL Name:\s*(\S+)', result.stdout)


def system_dll(name):
    value = name.lower()
    return value in SYSTEM_DLLS or value.startswith(('api-ms-win-', 'ext-ms-win-'))


def package_owners(packages, archive_dir, wanted):
    owners = {}
    legal_files = {}
    for package in packages:
        archive_path = archive_dir / package['filename']
        if not archive_path.is_file() or digest(archive_path) != package['sha256']:
            raise RuntimeError(f'Missing or unverified SDK archive: {archive_path}')
        if archive_path.name.endswith('.zst'):
            process = subprocess.Popen(['zstd', '-dc', str(archive_path)], stdout=subprocess.PIPE)
            archive = tarfile.open(fileobj=process.stdout, mode='r|')
        else:
            process = None
            archive = tarfile.open(archive_path, mode='r|*')
        try:
            with archive:
                for item in archive:
                    filename = Path(item.name).name.lower()
                    if item.isfile() and item.name.startswith('mingw64/share/') and filename.startswith(('copying', 'copyright', 'license')):
                        legal_files.setdefault(package['name'], []).append(item.name)
                    if item.name in wanted:
                        previous = owners.get(item.name)
                        if previous and previous != package['name']:
                            raise RuntimeError(f'Ambiguous package ownership: {item.name}')
                        owners[item.name] = package['name']
            if process and process.wait() != 0:
                raise RuntimeError(f'Cannot read archive: {archive_path}')
        finally:
            if process and process.poll() is None:
                process.kill()
                process.wait()
    missing = wanted - owners.keys()
    if missing:
        raise RuntimeError('Files absent from locked SDK archives: ' + ', '.join(sorted(missing)))
    return owners, legal_files


def copy_file(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, type=Path, help='Built bananaDesk.exe')
    parser.add_argument('--extra-executable', action='append', default=[], type=Path,
                        help='Additional local executable, e.g. codec self-test (repeatable)')
    parser.add_argument('--sdk', required=True, type=Path, help='Extracted SDK mingw64 directory')
    parser.add_argument('--lock', required=True, type=Path, help='sdk-packages.lock.json')
    parser.add_argument('--packages', type=Path, help='Package archive directory (default: beside lock)')
    parser.add_argument('--overlay', type=Path, help='Prefer custom DLLs from this directory or its bin/ subdirectory')
    parser.add_argument('--output', required=True, type=Path, help='Portable runtime directory')
    parser.add_argument('--objdump', default=shutil.which('x86_64-w64-mingw32-objdump') or shutil.which('objdump'))
    args = parser.parse_args()
    if not args.objdump:
        parser.error('objdump is required; pass --objdump /path/to/objdump')
    sdk, output, executable = args.sdk.resolve(), args.output.resolve(), args.exe.resolve()
    additional = [path.resolve() for path in args.extra_executable]
    application_paths = {executable, *additional}
    if len({'bananaDesk.exe', *(path.name.lower() for path in additional)}) != 1 + len(additional):
        raise RuntimeError('Additional executable filenames must be unique and different from bananaDesk.exe')
    lock_path = args.lock.resolve()
    lock = json.loads(lock_path.read_text())
    archive_dir = (args.packages or lock_path.parent / 'packages').resolve()
    packages = {p['name']: p for p in lock['packages']}
    bins = {path.name.lower(): path for path in (sdk / 'bin').glob('*.dll')}
    overlay = args.overlay.resolve() if args.overlay else None
    overlay_metadata = None
    if overlay:
        if not overlay.is_dir():
            raise RuntimeError('Overlay directory missing: ' + str(overlay))
        for directory in (overlay, overlay / 'bin'):
            bins.update({path.name.lower(): path for path in directory.glob('*.dll')})
        metadata_path = overlay / 'dependency-manifest.json'
        if metadata_path.is_file():
            overlay_metadata = json.loads(metadata_path.read_text())
        else:
            overlay_metadata = {'provenance': 'Local overlay; build provenance not supplied.'}
    # Qt5Network loads OpenSSL dynamically; PE imports alone cannot find it.
    seeds = [(executable, Path('bananaDesk.exe')),
             (sdk / 'bin/libssl-3-x64.dll', Path('libssl-3-x64.dll')),
             (sdk / 'bin/libcrypto-3-x64.dll', Path('libcrypto-3-x64.dll')),
             (sdk / 'share/qt5/plugins/platforms/qwindows.dll', Path('platforms/qwindows.dll')),
             (sdk / 'share/qt5/plugins/imageformats/qjpeg.dll', Path('imageformats/qjpeg.dll'))]
    seeds.extend((path, Path(path.name)) for path in additional)
    planned = {}
    queue = list(seeds)
    while queue:
        source, relative = queue.pop()
        key = relative.name.lower()
        if key in planned:
            continue
        if not source.is_file():
            raise RuntimeError(f'Missing required runtime file: {source}')
        imports = inspect_pe(source, args.objdump)
        planned[key] = {'source': source, 'relative': relative, 'imports': imports}
        for dependency in imports:
            if system_dll(dependency):
                continue
            target = bins.get(dependency.lower())
            if target is None:
                raise RuntimeError(f'Unresolved non-system import: {source.name} -> {dependency}')
            queue.append((target, Path(target.name)))

    wanted = {p['source'].relative_to(sdk.parent).as_posix()
              for p in planned.values() if p['source'].is_relative_to(sdk)}
    owners, legal_files = package_owners(lock['packages'], archive_dir, wanted)
    used_packages = set(owners.values())
    output.mkdir(parents=True, exist_ok=True)
    # Remove only files generated by an earlier run of this same packager.
    previous_manifest = output / 'runtime-manifest.json'
    if previous_manifest.exists():
        previous = json.loads(previous_manifest.read_text())
        new_files = {str(p['relative']) for p in planned.values()}
        for old in previous.get('files', []):
            path = output / old['path']
            if old['path'] not in new_files and path.resolve().is_relative_to(output):
                path.unlink(missing_ok=True)
    manifest_files = []
    for entry in sorted(planned.values(), key=lambda e: str(e['relative'])):
        source, relative = entry['source'], entry['relative']
        copy_file(source, output / relative)
        archive_name = source.relative_to(sdk.parent).as_posix() if source.is_relative_to(sdk) else None
        owner = owners[archive_name] if archive_name else ('bananaDesk application' if source in application_paths else 'Local FFmpeg overlay')
        manifest_files.append({'path': relative.as_posix(), 'bytes': source.stat().st_size,
                               'sha256': digest(output / relative), 'imports': entry['imports'],
                               'package': owner})
    (output / 'qt.conf').write_text('[Paths]\nPlugins=.\n', encoding='utf-8')

    notices = Path(__file__).resolve().parent.parent / 'third_party'
    common = notices / 'licenses'
    if not common.is_dir():
        raise RuntimeError('Common third-party license texts missing: ' + str(common))
    shutil.copytree(common, output / 'licenses/common', dirs_exist_ok=True)
    missing_license_directories = []
    license_packages = []
    for name in sorted(used_packages):
        package = packages[name]
        short_name = name.removeprefix('mingw-w64-x86_64-')
        directory = sdk / 'share/licenses' / short_name
        if directory.is_dir():
            shutil.copytree(directory, output / 'licenses' / short_name, dirs_exist_ok=True)
        elif legal_files.get(name):
            # Some packages (notably ICU) put notices in share/<name>/<version>/.
            for archive_path in legal_files[name]:
                source = sdk.parent / archive_path
                relative = Path(archive_path).relative_to('mingw64/share')
                copy_file(source, output / 'licenses' / short_name / relative)
        else:
            missing_license_directories.append(name)
        package_record = {key: value for key, value in package.items()}
        package_record['included_runtime_files'] = [p['path'] for p in manifest_files if p['package'] == name]
        package_record['license_directory'] = ('licenses/' + short_name) if directory.is_dir() or legal_files.get(name) else None
        package_record['packaging_recipe_repository'] = 'https://github.com/msys2/MINGW-packages'
        license_packages.append(package_record)
    copy_file(notices / 'THIRD-PARTY-NOTICES.md', output / 'THIRD-PARTY-NOTICES.md')
    if overlay and (overlay / 'licenses').is_dir():
        shutil.copytree(overlay / 'licenses', output / 'licenses/local-overlay', dirs_exist_ok=True)
    (output / 'dependency-manifest.json').write_text(json.dumps({
        'repository_database_sha256': lock.get('repository_database_sha256'),
        'packages': license_packages,
        'packages_without_package_license_directory': missing_license_directories,
        'common_license_texts': sorted(p.name for p in common.iterdir() if p.is_file()),
        'local_overlay': overlay_metadata,
        'note': 'License labels are preserved from the official package database; they are not a relicensing declaration.'
    }, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    report = {'target': 'Windows 10 x64', 'executable': 'bananaDesk.exe',
              'additional_executables': [path.name for path in additional],
              'dll_count': len(planned) - len(application_paths), 'runtime_bytes': sum(f['bytes'] for f in manifest_files),
              'dependency_package_count': len(used_packages),
              'local_overlay_files': [p['path'] for p in manifest_files if p['package'] == 'Local FFmpeg overlay'],
              'explicit_dynamic_dependencies': ['libssl-3-x64.dll', 'libcrypto-3-x64.dll'],
              'plugin_paths': ['platforms/qwindows.dll', 'imageformats/qjpeg.dll'],
              'files': manifest_files, 'unresolved_non_system_imports': [],
              'validation_scope': 'Static x64 PE import closure and file hashes; no claim of physical Windows runtime testing.'}
    # Validate the assembled directory, including plugin imports, independently of SDK lookup.
    assembled = {Path(f['path']).name.lower() for f in manifest_files}
    for entry in manifest_files:
        for dependency in inspect_pe(output / entry['path'], args.objdump):
            if not system_dll(dependency) and dependency.lower() not in assembled:
                raise RuntimeError(f'Packaged import missing: {entry["path"]} -> {dependency}')
    (output / 'runtime-manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in report.items() if key != 'files'}, indent=2))
    print('Package directory: ' + str(output))


if __name__ == '__main__':
    main()
