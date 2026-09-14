#!/usr/bin/env python3
"""Verify real application disconnect-to-lock wiring on a private bus and Xvfb."""
import argparse, hashlib, json, os, signal, socket, ssl, subprocess, sys, time
from pathlib import Path
import fps_benchmark as fps

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('binary',type=Path);p.add_argument('lock_test_binary',type=Path);p.add_argument('--artifacts',required=True,type=Path)
a=p.parse_args();a.binary=a.binary.resolve();a.lock_test_binary=a.lock_test_binary.resolve()
a.artifacts=a.artifacts.resolve();a.artifacts.mkdir(parents=True,exist_ok=True)
run=fps.Run(a.artifacts);report={'passed':False,'scope':'Real Linux application, isolated Xvfb and private mock screen-lock D-Bus; no real desktop lock'}
checks=[];invitations=[]
def connect(invite):
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT);context.check_hostname=False;context.verify_mode=ssl.CERT_NONE
    connection=context.wrap_socket(socket.create_connection((invite['host'],invite['port']),timeout=5),server_hostname=invite['host'])
    assert hashlib.sha256(connection.getpeercert(binary_form=True)).hexdigest()==invite['pin']
    return connection
def launch(command,env,name):
    if '--host' in command:
        # Never run an auto-locking test host against the user's real session bus.
        assert env['DBUS_SESSION_BUS_ADDRESS'].startswith('unix:path='+str(a.artifacts/'bus'))
    handle=(a.artifacts/(name+'.log')).open('w');run.handles.append(handle)
    process=subprocess.Popen([str(x) for x in command],env=env,stdout=handle,stderr=subprocess.STDOUT)
    run.processes.append(process);return process
try:
    env=run.xvfb('xvfb')
    bus=subprocess.Popen(['dbus-daemon','--session','--nofork','--print-address=1','--address=unix:path='+str(a.artifacts/'bus')],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    run.processes.append(bus)
    address=bus.stdout.readline().decode().strip()
    assert address.startswith('unix:path='+str(a.artifacts/'bus'))
    env['DBUS_SESSION_BUS_ADDRESS']=address
    mock=launch([a.lock_test_binary,'--lock-mock','org.gnome.ScreenSaver','ok'],env,'mock')
    run.wait(lambda:'READY' in (a.artifacts/'mock.log').read_text())
    def count():return (a.artifacts/'mock.log').read_text().count('LOCK org.gnome.ScreenSaver')
    for enabled in (True,False):
        name='enabled' if enabled else 'disabled';inv=a.artifacts/(name+'.invitation');invitations.append(inv);inv.unlink(missing_ok=True)
        cmd=[a.binary,'--host','--view-only','--bind','127.0.0.1','--port',str(fps.free_port()),'--invite-file',inv,'--codec','jpeg']
        if not enabled:cmd.append('--no-lock-on-disconnect')
        host=launch(cmd,env,name);run.wait(inv.exists);iv=fps.invitation(inv)
        before=count();sock=connect(iv);sock.close();time.sleep(.3);assert count()==before
        checks.append(name+': unauthenticated disconnect does not lock')
        sock=connect(iv);receiver=fps.Receiver(sock)
        fps.send(sock,'A',{'v':1,'token':iv['token'],'codecs':['jpeg'],'window':1})
        message=receiver.receive(5);assert message and message[0]=='W'
        message=receiver.receive(5);assert message and message[0]=='F'
        sock.close()
        if enabled:run.wait(lambda:count()==before+1)
        else:time.sleep(.5);assert count()==before
        host.send_signal(signal.SIGTERM);host.wait(timeout=5);run.processes.remove(host);time.sleep(.1)
        assert count()==before+int(enabled)
        checks.append(name+': authenticated disconnect '+('requests one OS lock; later shutdown does not duplicate it' if enabled else 'respects the disabled setting'))
    report['passed']=True
finally:
    run.close()
    for inv in invitations:inv.unlink(missing_ok=True)
    report['checks']=checks;(a.artifacts/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
