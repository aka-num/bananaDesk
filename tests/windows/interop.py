#!/usr/bin/env python3
"""Local Linux/Wine-PE interoperability checks. This is not a Windows hardware test."""
import argparse, base64, ctypes, hashlib, importlib.util, json, os, signal, socket, ssl, struct, subprocess, sys, time
from pathlib import Path

HERE=Path(__file__).resolve().parent
ROOT=Path(os.environ.get('LANDESK_WINDOWS_TEST_DIR',HERE)).resolve()
PROJECT=Path(os.environ.get('LANDESK_SOURCE_ROOT',HERE.parent.parent)).resolve()
WINE=os.environ.get('LANDESK_WINE',str(ROOT/'wine11'))
spec=importlib.util.spec_from_file_location('fps_helpers',PROJECT/'tests/fps_benchmark.py')
fps=importlib.util.module_from_spec(spec);spec.loader.exec_module(fps)

class Run(fps.Run):
    def launch(self, command, env, name, pass_fds=()):
        if "--host" in command and "--no-lock-on-disconnect" not in command:
            command = list(command) + ["--no-lock-on-disconnect"]
        handle=(self.directory/(name+'.log')).open('w');self.handles.append(handle)
        p=subprocess.Popen([str(x) for x in command],env=env,stdout=handle,stderr=subprocess.STDOUT,pass_fds=pass_fds,start_new_session=True)
        self.processes.append(p);return p
    def stop(self,p):
        if p.poll() is None:
            os.killpg(p.pid,signal.SIGTERM)
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=5)
        if p in self.processes:self.processes.remove(p)

def winpath(path):return 'Z:'+str(Path(path).resolve()).replace('/','\\')
def events(path):
    try:return [json.loads(x) for x in path.read_text().splitlines() if x]
    except FileNotFoundError:return []
def invite(path):return fps.invitation(path)
def tls(iv):
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT);ctx.check_hostname=False;ctx.verify_mode=ssl.CERT_NONE;ctx.minimum_version=ssl.TLSVersion.TLSv1_2
    s=ctx.wrap_socket(socket.create_connection((iv['host'],iv['port']),timeout=4),server_hostname=iv['host'])
    assert hashlib.sha256(s.getpeercert(binary_form=True)).hexdigest()==iv['pin'];return s

def input_roundtrip(run,iv,ev,windows):
    initial=events(ev);origin=next(x['origin'] for x in initial if x['kind']=='ready') if windows else [80,90]
    pt={'x':(origin[0]+200)/1919,'y':(origin[1]+300)/1079}
    s=tls(iv);r=fps.Receiver(s);fps.send(s,'A',{'v':1,'token':iv['token'],'codecs':['h264','jpeg'],'window':1})
    m=r.receive(5);assert m and m[0]=='W';welcome=json.loads(m[1]);assert welcome['codec']=='h264' and welcome['control']
    m=r.receive(5);assert m and m[0]=='F' and len(m[1])>20
    for kind,extra in [('button',{'button':1,'down':True}),('button',{'button':1,'down':False}),('key',{'key':65,'down':True}),('key',{'key':65,'down':False}),('wheel',{'steps':1})]:
        fps.send(s,'I',{'kind':kind,**pt,**extra})
    run.wait(lambda:any(x['kind']=='wheel' for x in events(ev)[len(initial):]))
    seen=events(ev)[len(initial):]
    assert any(x['kind']=='key_down' and x.get('a',x.get('key'))==65 for x in seen),seen
    assert any(x['kind']=='key_up' and x.get('a',x.get('key'))==65 for x in seen),seen
    assert any(x['kind']=='mouse_down' and abs(x.get('a',x.get('x'))-200)<=2 and abs(x.get('b',x.get('y'))-300)<=2 for x in seen),seen
    before=len(events(ev));fps.send(s,'I',{'kind':'key','key':0x01000021,'down':True})
    ctrl=17 if windows else 0x01000021
    run.wait(lambda:any(x['kind']=='key_down' and x.get('a',x.get('key'))==ctrl for x in events(ev)[before:]))
    s.close()
    run.wait(lambda:any(x['kind']=='key_up' and x.get('a',x.get('key'))==ctrl for x in events(ev)[before:]))
    return {'tls_pin_verified':True,'h264_frame_received':True,'mouse_coordinate_verified':True,'key_press_release_verified':True,'wheel_verified':True,'disconnect_releases_ctrl':True}

def screenshot(env,path):fps.screenshot(env,path)
def click_type_display(env,image_path):
    # Operate only the isolated test display. Pick a blue synthetic desktop pixel.
    from PIL import Image
    image=Image.open(image_path).convert('RGB');points=[]
    for y in range(200,image.height-80,10):
        for x in range(120,image.width-100,10):
            r,g,b=image.getpixel((x,y))
            if abs(r-37)<15 and abs(g-99)<15 and abs(b-235)<15:points.append((x,y))
    assert len(points)>100,'Viewer screenshot did not show the synthetic blue desktop'
    x,y=points[len(points)//2]
    x11=ctypes.CDLL('libX11.so.6');xtst=ctypes.CDLL('libXtst.so.6')
    x11.XOpenDisplay.argtypes=[ctypes.c_char_p];x11.XOpenDisplay.restype=ctypes.c_void_p
    x11.XFlush.argtypes=[ctypes.c_void_p];x11.XCloseDisplay.argtypes=[ctypes.c_void_p]
    x11.XKeysymToKeycode.argtypes=[ctypes.c_void_p,ctypes.c_ulong];x11.XKeysymToKeycode.restype=ctypes.c_uint
    xtst.XTestFakeMotionEvent.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_ulong]
    xtst.XTestFakeButtonEvent.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_int,ctypes.c_ulong]
    xtst.XTestFakeKeyEvent.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_int,ctypes.c_ulong]
    display=x11.XOpenDisplay(env['DISPLAY'].encode());assert display
    xtst.XTestFakeMotionEvent(display,-1,x,y,0);xtst.XTestFakeButtonEvent(display,1,1,0);xtst.XTestFakeButtonEvent(display,1,0,0);x11.XFlush(display);time.sleep(.2)
    key=x11.XKeysymToKeycode(display,ord('a'));xtst.XTestFakeKeyEvent(display,key,1,0);xtst.XTestFakeKeyEvent(display,key,0,0);x11.XFlush(display);x11.XCloseDisplay(display)
    return [x,y]

def retain_synthetic_crop(path):
    # Retain only the synthetic desktop region, excluding all invitation fields.
    from PIL import Image
    image=Image.open(path).convert('RGB');points=[]
    for y in range(0,image.height,2):
        for x in range(0,image.width,2):
            r,g,b=image.getpixel((x,y))
            if abs(r-37)<15 and abs(g-99)<15 and abs(b-235)<15:points.append((x,y))
    if points:
        image.crop((min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points)+1,max(p[1] for p in points)+1)).save(path.with_name('synthetic-desktop.png'))
    path.unlink(missing_ok=True)

def direction(args,name,windows_host):
    directory=ROOT/'shared'/name;directory.mkdir(parents=True,exist_ok=True)
    run=Run(directory);report={'direction':name,'scope':'Windows x64 PE under Wine 11.0 and native Linux X11, loopback TLS; not Windows hardware','passed':False,'limitations':['Wine compatibility layer, not physical Windows or LAN','Functional pass does not require the 60 fps target to be achieved']}
    ivfile=directory/'invitation';stats=directory/'stats.json';ev=directory/'events.jsonl'
    for p in (ivfile,stats,ev):p.unlink(missing_ok=True)
    wine_env=os.environ.copy();wine_display=os.environ.get('LANDESK_WINE_DISPLAY') or ':'+(ROOT/'display.txt').read_text().strip();wine_env['LANDESK_WINE_DISPLAY']=wine_display
    try:
        linux_env=run.xvfb('linux-xvfb');wine_ui_env=linux_env.copy();wine_ui_env['DISPLAY']=wine_display
        host_args=['--host','--bind','127.0.0.1','--port',str(fps.free_port()),'--invite-file',winpath(ivfile) if windows_host else str(ivfile),'--fps','60','--codec','h264']
        if windows_host:host_args+=['--log-file',winpath(directory/'windows-host.log')]
        host=run.launch(([WINE,args.windows] if windows_host else [args.linux])+host_args,wine_env if windows_host else linux_env,'host')
        run.wait(ivfile.exists,timeout=25)
        if windows_host:probe=run.launch([WINE,ROOT/'winapi_desktop_probe.exe',winpath(ev)],wine_env,'probe')
        else:probe=run.launch([sys.executable,HERE/'linux_animated_probe.py',ev],linux_env,'probe')
        run.wait(lambda:any(x['kind']=='ready' for x in events(ev)),timeout=15)
        # Native application viewer validates actual Qt decode and paint on the opposite platform.
        client_args=['--connect-file',str(ivfile) if windows_host else winpath(ivfile),'--stats-file',str(stats) if windows_host else winpath(stats)]
        if not windows_host:client_args+=['--log-file',winpath(directory/'windows-client.log')]
        client=run.launch(([args.linux] if windows_host else [WINE,args.windows])+client_args,linux_env if windows_host else wine_env,'client')
        run.wait(lambda:fps.latest(stats).get('painted_frames',0)>=30,timeout=30)
        first=fps.latest(stats);run.wait(lambda:fps.latest(stats).get('elapsed_seconds',0)-first['elapsed_seconds']>=5,timeout=20);last=fps.latest(stats)
        assert last['codec']=='h264' and last['target_fps']==60,last
        elapsed=last['elapsed_seconds']-first['elapsed_seconds']
        report['video']={'codec':last['codec'],'target_fps':60,'seconds':elapsed,'decoded_fps':(last['decoded_frames']-first['decoded_frames'])/elapsed,'painted_fps':(last['painted_frames']-first['painted_frames'])/elapsed,'received_bytes':last['bytes_received']-first['bytes_received'],'distinct_decoded_fps':(last['distinct_decoded_frames']-first['distinct_decoded_frames'])/elapsed,'stats':last}
        viewer_env=linux_env if windows_host else wine_ui_env
        screenshot(viewer_env,directory/'viewer.png')
        before=len(events(ev));report['viewer_click']=click_type_display(viewer_env,directory/'viewer.png')
        run.wait(lambda:any(x['kind']=='key_down' and x.get('a',x.get('key'))==65 for x in events(ev)[before:]),timeout=8)
        run.wait(lambda:any(x['kind']=='key_up' and x.get('a',x.get('key'))==65 for x in events(ev)[before:]),timeout=8)
        report['native_client_ui_to_remote_key']=True
        retain_synthetic_crop(directory/'viewer.png')
        run.stop(client);time.sleep(.5)
        report['protocol_to_native_input']=input_roundtrip(run,invite(ivfile),ev,windows_host)
        report['backpressure']=fps.slow_receiver(invite(ivfile),3)
        report['passed']=True
    except Exception as e:
        report['error']=repr(e)
        for log in directory.glob('*.log'):print(log.name+':\n'+log.read_text(errors='replace')[-2500:],file=sys.stderr)
    finally:
        run.close();ivfile.unlink(missing_ok=True)
        if (directory/'viewer.png').exists():retain_synthetic_crop(directory/'viewer.png')
        (directory/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(report,ensure_ascii=False,indent=2),flush=True);return report

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('linux',type=Path);parser.add_argument('windows',type=Path);parser.add_argument('--direction',choices=['both','windows-host','windows-client'],default='both');args=parser.parse_args()
    args.linux=args.linux.resolve();args.windows=args.windows.resolve();reports=[]
    if args.direction in ('both','windows-client'):reports.append(direction(args,'windows-client-linux-host',False))
    if args.direction in ('both','windows-host'):reports.append(direction(args,'linux-client-windows-host',True))
    (ROOT/'shared'/'interop-report.json').write_text(json.dumps({'passed':all(r['passed'] for r in reports),'reports':reports},ensure_ascii=False,indent=2)+'\n')
    return 0 if all(r['passed'] for r in reports) else 1
if __name__=='__main__':raise SystemExit(main())
