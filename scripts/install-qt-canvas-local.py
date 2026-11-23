"""Install the official Qt 6.11.2 MSVC SDK inside build-dev (SHA-1 verified archives)."""
from pathlib import Path, PurePosixPath
import concurrent.futures, hashlib, subprocess, sys, urllib.request, xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parent.parent
BUILD=ROOT/'build-dev'
sys.path.insert(0,str(BUILD/'aqt-tools'))
import py7zr
BASE='https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6112/qt6_6112_msvc2022_64/'
modules={'qtbase','qtsvg','qtdeclarative','qttools','qtcharts','qtmultimedia','qtcanvaspainter','qtshadertools','qtimageformats'}
xml=urllib.request.urlopen(BASE+'Updates.xml',timeout=45).read()
(BUILD/'qt611-Updates.xml').write_bytes(xml)
packages=[]
for p in ET.fromstring(xml).findall('PackageUpdate'):
 name=p.findtext('Name','')
 if not name.endswith('.win64_msvc2022_64') or 'debug' in name: continue
 for archive in p.findtext('DownloadableArchives','').split(','):
  archive=archive.strip()
  if archive.split('-')[0] not in modules: continue
  packages.append((archive,BASE+name+'/'+p.findtext('Version')+archive))
cache=BUILD/'qt611-downloads';cache.mkdir(exist_ok=True)
def download(pair):
 name,url=pair;dest=cache/name
 expected=urllib.request.urlopen(url+'.sha1',timeout=45).read().decode().split()[0].lower()
 if not dest.exists() or hashlib.sha1(dest.read_bytes()).hexdigest()!=expected:
  with urllib.request.urlopen(url,timeout=90) as response, dest.open('wb') as out:
   while block:=response.read(1024*1024): out.write(block)
 if hashlib.sha1(dest.read_bytes()).hexdigest()!=expected: raise RuntimeError('Checksum mismatch: '+name)
 print('Verified '+name,flush=True)
 return dest
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool: archives=list(pool.map(download,packages))
prefix=BUILD/'qt-sdk'/'6.11.2'/'msvc2022_64';prefix.mkdir(parents=True,exist_ok=True)
for archive in archives:
 with py7zr.SevenZipFile(archive) as z:
  names=z.getnames()
  if any(PurePosixPath(n.replace('\\','/')).is_absolute() or '..' in PurePosixPath(n.replace('\\','/')).parts or ':' in n for n in names): raise RuntimeError('Unsafe archive path')
  nested=any(n.startswith('6.11.2/') for n in names)
 destination=BUILD/'qt-sdk' if nested else prefix
 subprocess.run([str(BUILD/'bin/support/7zip/7z.exe'),'x','-y',str(archive),'-o'+str(destination)],check=True,stdout=subprocess.DEVNULL)
 print('Extracted '+archive.name,flush=True)
print('Qt SDK: '+str(prefix),flush=True)
