from pathlib import Path
import json, subprocess, re, sys
root=Path(__file__).resolve().parent.parent
build=root/'build-dev'
ninja=Path('C:/Users/powww/AppData/Local/Python/pythoncore-3.14-64/Scripts/ninja.exe')
entries=json.loads(subprocess.check_output([str(ninja),'-t','compdb'],cwd=build,text=True))
wanted=set(sys.argv[1:]) or {'ChannelView.cpp','TwitchIrcServer.cpp','TwitchReadConnectionPool.cpp','RatelimitBucket.cpp'}
qt=str(build/'qt-sdk/6.11.2/msvc2022_64').replace('\\','/')
for entry in entries:
 name=Path(entry['file']).name
 if name not in wanted: continue
 command=entry['command']
 command=re.sub(r'/(?:Yu|Yc|Fp|FI)(?:"[^"]*"|\S+)','',command)
 command=command.replace('C:\\Qt\\6.9.3\\msvc2022_64',qt).replace('C:/Qt/6.9.3/msvc2022_64',qt)
 command=command.replace('\\6.9.3\\','\\6.11.2\\').replace('/6.9.3/','/6.11.2/')
 command=command.replace('/nologo', '/nologo /Zs /DCHATTERINO_CANVAS_PAINTER /I'+qt+'/include/QtCanvasPainter')
 command=command.replace('/showIncludes','')
 print('Syntax check: '+name,flush=True)
 subprocess.run(command,cwd=build,shell=True,check=True)
 wanted.remove(name)
if wanted: raise RuntimeError('Missing compile commands: '+str(wanted))
print('All requested integration units passed syntax checks.',flush=True)
