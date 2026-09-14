#!/usr/bin/env python3
"""Targeted Windows-PE runtime authorization checks; Wine compatibility, not hardware."""
import argparse,json,os,socket,ssl,time,sys
from pathlib import Path
from interop import ROOT,PROJECT,WINE,Run,winpath,events,invite,tls,fps

def rejected(sock):
    sock.settimeout(3)
    try:assert sock.recv(1)==b''
    except (ssl.SSLError,ConnectionResetError):pass
    finally:sock.close()

def main():
    p=argparse.ArgumentParser();p.add_argument('linux',type=Path);p.add_argument('windows',type=Path);a=p.parse_args();a.linux=a.linux.resolve();a.windows=a.windows.resolve()
    directory=ROOT/'shared/security';directory.mkdir(parents=True,exist_ok=True);run=Run(directory);checks=[];report={'passed':False,'scope':'Windows x64 PE under Wine11, isolated Xvfb and loopback TLS'}
    inv=directory/'invitation';ev=directory/'events.jsonl';wrong=directory/'wrong-pin';linux_inv=directory/'linux-invitation'
    for f in (inv,ev,wrong,linux_inv):f.unlink(missing_ok=True)
    wine_env=os.environ.copy();wine_env['LANDESK_WINE_DISPLAY']=os.environ.get('LANDESK_WINE_DISPLAY') or ':'+(ROOT/'display.txt').read_text().strip()
    try:
        linux_env=run.xvfb('linux-xvfb')
        def host(viewonly=False):
            cmd=[WINE,a.windows,'--host','--bind','127.0.0.1','--port',str(fps.free_port()),'--invite-file',winpath(inv),'--log-file',winpath(directory/('host-viewonly.log' if viewonly else 'host.log'))]
            if viewonly:cmd+=['--view-only']
            h=run.launch(cmd,wine_env,'host-stdout');run.wait(inv.exists,timeout=20);return h
        h=host()
        probe=run.launch([WINE,ROOT/'winapi_desktop_probe.exe',winpath(ev)],wine_env,'probe')
        run.wait(lambda:any(e['kind']=='ready' for e in events(ev)))
        iv=invite(inv);s=tls(iv);s.settimeout(.4)
        try:s.recv(1);raise AssertionError('Windows host sent data before authentication')
        except socket.timeout:pass
        finally:s.close()
        checks.append('Windows host sends no desktop before authentication');time.sleep(.2)
        s=tls(iv);fps.send(s,'A',{'v':1,'token':'0'*48});rejected(s);checks.append('Windows host rejects wrong access token');time.sleep(.2)
        before=len(events(ev));s=tls(iv);fps.send(s,'I',{'kind':'key','key':65,'down':True});rejected(s);time.sleep(.2)
        assert len(events(ev))==before;checks.append('Windows host rejects input before authentication without native input delivery')
        run.stop(h);run.stop(probe);inv.unlink();ev.unlink();h=host(True)
        probe=run.launch([WINE,ROOT/'winapi_desktop_probe.exe',winpath(ev)],wine_env,'viewonly-probe')
        run.wait(lambda:any(e['kind']=='ready' for e in events(ev)))
        iv=invite(inv)
        for attempted in ({'kind':'key','key':65,'down':True},{'kind':'button','x':.2,'y':.3,'button':1,'down':True},{'kind':'wheel','x':.2,'y':.3,'steps':1}):
            s=tls(iv);r=fps.Receiver(s);fps.send(s,'A',{'v':1,'token':iv['token'],'codecs':['h264','jpeg'],'window':1})
            msg=r.receive(5);assert msg and msg[0]=='W';welcome=json.loads(msg[1]);assert welcome['control'] is False
            msg=r.receive(5);assert msg and msg[0]=='F'
            before=len(events(ev));fps.send(s,'I',attempted);rejected(s);time.sleep(.2);assert len(events(ev))==before
        checks.append('Windows view-only host streams video and rejects mouse/key/wheel injection by closing the session without native input delivery');run.stop(h);run.stop(probe)
        lh=run.launch([a.linux,'--host','--bind','127.0.0.1','--port',str(fps.free_port()),'--invite-file',linux_inv],linux_env,'linux-host');run.wait(linux_inv.exists)
        iv=invite(linux_inv);iv['pin']='0'*64
        import base64
        wrong.write_text('landesk1:'+base64.urlsafe_b64encode(json.dumps(iv).encode()).decode().rstrip('='))
        logfile=directory/'windows-wrong-pin.log';logfile.unlink(missing_ok=True)
        viewer=run.launch([WINE,a.windows,'--connect-file',winpath(wrong),'--log-file',winpath(logfile)],wine_env,'windows-viewer-stdout')
        run.wait(lambda:logfile.exists() and '证书指纹不匹配' in logfile.read_text(errors='replace'),timeout=20)
        assert '已连接 ·' not in logfile.read_text(errors='replace');checks.append('Actual Windows client rejects mismatched certificate fingerprint before authentication')
        report['passed']=True
    except Exception as e:
        report['error']=repr(e)
        for log in directory.glob('*.log'):print(log.name+':\n'+log.read_text(errors='replace')[-2000:],file=sys.stderr)
    finally:
        run.close()
        for f in (inv,wrong,linux_inv):f.unlink(missing_ok=True)
        report['checks']=checks;(directory/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n');print(json.dumps(report,ensure_ascii=False,indent=2),flush=True)
    return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
