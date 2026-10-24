#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Live IPv4/IPv6 two-way EIGRP adjacency qualification (FRR + CSR1000v).

Native netlab deploys configuration using the shared named-mode template. The
Python driver validates actual peers, not configured text or transmitted HELLOs.
The remote runner owns teardown on success and failure.
"""
import argparse
import datetime as dt
import json
import os
import pathlib
import pty
import re
import select
import shutil
import subprocess
import sys
import time

import yaml

ERROR = re.compile(r"(?:%\s*(?:Invalid input|Incomplete command|Ambiguous command|Error)|unknown command|ambiguous command|failed to connect)", re.I)
PROMPT = re.compile(r"(?m)(?:^|\n)[A-Za-z][\w.():-]*[>#]\s*$")


def command(args, timeout=30, input_data=None, check=True, env=None, log_file=None):
    p = subprocess.run(args, input=input_data, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=timeout, env=env)
    if log_file is not None:
        pathlib.Path(log_file).write_text(p.stdout, encoding='utf8')
    if check and p.returncode:
        raise RuntimeError(f"{' '.join(args)} exited {p.returncode}: {p.stdout[-3000:]}")
    return p.stdout


def assert_cli(output, label):
    if ERROR.search(output):
        raise RuntimeError(f"{label}: command rejected: {output[-3000:]}")


def ipv4_peer(output, peer):
    # EIGRP's neighbor table has an IPv4 address as a field. Reject header-only
    # output, locally echoed 'show' commands, and adjacent partial IP matches.
    return bool(re.search(r"(?<![0-9.])" + re.escape(peer) + r"(?![0-9.])", output))


def ipv6_peer(output, peer):
    # The link-local value comes from the actual *other* router interface.
    return bool(re.search(r"(?<![0-9a-f:])" + re.escape(peer.lower()) + r"(?![0-9a-f:])", output.lower()))


def extract_link_local(output):
    match = re.search(r"(?i)\bfe80:(?:[0-9a-f]{0,4}:){1,6}[0-9a-f]{1,4}(?:%\w+)?\b", output)
    if not match:
        raise RuntimeError("No IPv6 link-local address in interface output")
    return match.group().split('%')[0].lower()


def get_csr_mgmt_ip(name):
    data = json.loads(command(['docker', 'inspect', name]))[0]
    nets = data['NetworkSettings']['Networks']
    for entry in nets.values():
        if entry.get('IPAddress'):
            return entry['IPAddress']
    raise RuntimeError(f'No management IPv4 address for {name}')


class Cisco:
    """Read-only IOS XE CLI through SSH with a PTY, without Paramiko deps."""
    def __init__(self, host, log_file):
        self.host = host
        self.log_file = log_file
        self.fd = None
        self.process = None

    def __enter__(self):
        master, slave = pty.openpty()
        env = os.environ.copy()
        env['SSHPASS'] = 'admin'  # vrnetlab CSR default management user
        argv = ['sshpass', '-e', 'ssh', '-tt', '-o', 'ConnectTimeout=8',
                '-o', 'StrictHostKeyChecking=accept-new',
                '-o', f'UserKnownHostsFile={self.log_file.parent / "known_hosts"}',
                '-o', 'LogLevel=ERROR', '-o', 'NumberOfPasswordPrompts=1',
                f'admin@{self.host}']
        self.process = subprocess.Popen(argv, stdin=slave, stdout=slave,
                                        stderr=slave, env=env, close_fds=True)
        os.close(slave)
        self.fd = master
        try:
            self._read_prompt(25)
            self.run('terminal length 0')
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def _read_prompt(self, timeout):
        end = time.monotonic() + timeout
        buf = ''
        while time.monotonic() < end:
            ready, _, _ = select.select([self.fd], [], [], max(0.0, min(0.5, end - time.monotonic())))
            if ready:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    chunk = b''
                if not chunk:
                    raise RuntimeError(f'Cisco SSH disconnected: {buf[-1500:]}')
                buf += chunk.decode('utf-8', 'replace').replace('\r', '')
                if PROMPT.search(buf):
                    with self.log_file.open('a', encoding='utf8') as f:
                        f.write(buf + '\n')
                    return buf
                if len(buf) > 200000:
                    buf = buf[-150000:]
        raise TimeoutError(f'Cisco CLI prompt timeout: {buf[-2000:]}')

    def run(self, cmd):
        os.write(self.fd, (cmd + '\n').encode('utf8'))
        output = self._read_prompt(20)
        assert_cli(output, 'Cisco ' + cmd)
        return output

    def __exit__(self, *_):
        if self.fd is not None:
            try: os.write(self.fd, b'exit\n')
            except OSError: pass
            os.close(self.fd)
        if self.process is not None:
            try: self.process.wait(timeout=3)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()


def frr_cli(container, cmd, artifact_dir):
    output = command(['docker', 'exec', container, 'vtysh', '-d', 'eigrpd', '-c', cmd], timeout=15)
    assert_cli(output, cmd)
    with (artifact_dir / 'frr-cli.log').open('a', encoding='utf8') as f:
        f.write(f'\n$ {cmd}\n{output}\n')
    return output


def wait_frr(container, seconds=120):
    """Wait for the actual OpenEIGRP VTY socket, not just Docker startup.

    Containerlab considers a Linux node deployed before watchfrr has finished
    starting eigrpd. A successful docker exec is not a daemon readiness check.
    """
    deadline = time.monotonic() + seconds
    last = 'eigrpd has not been checked'
    while time.monotonic() < deadline:
        try:
            output = command(['docker', 'exec', container, 'vtysh', '-d', 'eigrpd',
                              '-c', 'show version'], timeout=12)
            assert_cli(output, 'eigrpd readiness')
            if not output.strip():
                raise RuntimeError('eigrpd returned an empty response')
            return
        except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as exc:
            last = str(exc)
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(3, remaining))
    raise TimeoutError('OpenEIGRP eigrpd CLI did not become ready: ' + last[-1800:])


def wait_csr(address, artifacts, deadline, process, asn):
    """Wait for netlab-configured CSR CLI without flooding terminal output.

    Transient SSH failures go to cisco-readiness.log; the final failure includes
    the last diagnostic. Readiness is not an EIGRP adjacency assertion.
    """
    last = ''
    attempts = 0
    log = artifacts / 'cisco-readiness.log'
    while time.monotonic() < deadline:
        attempts += 1
        try:
            with Cisco(address, artifacts / 'cisco-cli.log') as csr:
                running = csr.run('show running-config')
                for required in (f'router eigrp {process}',
                                 f'address-family ipv4 unicast autonomous-system {asn}',
                                 f'address-family ipv6 unicast autonomous-system {asn}'):
                    if required not in running:
                        raise RuntimeError(f'CSR named-mode configuration missing {required}')
                interface = csr.run('show ipv6 interface GigabitEthernet2')
                result = extract_link_local(interface)
                if attempts > 1:
                    print(f'Cisco ready after {attempts} connection attempts '
                          '(transient SSH messages saved in report)', flush=True)
                return result
        except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as exc:
            if isinstance(exc, RuntimeError) and str(exc).startswith('CSR named-mode configuration missing'):
                raise  # netlab claims provisioning complete; a missing config is not an SSH startup delay.
            last = str(exc)
            with log.open('a', encoding='utf8') as f:
                f.write(f'Attempt {attempts}: {last}\n')
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(8, remaining))
    raise TimeoutError(f'CSR IOS XE configuration not ready after {attempts} attempts: {last[-1500:]}')


def validate_frr(container, artifacts, process, asn):
    """Verify the deployed named-mode configuration and read its real LLA."""
    running = frr_cli(container, 'show running-config', artifacts)
    for required in (f'router eigrp {process}',
                     f'address-family ipv4 unicast autonomous-system {asn}',
                     f'address-family ipv6 unicast autonomous-system {asn}'):
        if required not in running:
            raise RuntimeError(f'FRR named-mode configuration missing {required}')
    return extract_link_local(command(['docker', 'exec', container, 'ip', '-6', '-o',
                                       'addr', 'show', 'dev', 'eth1', 'scope', 'link']))


def assess_family(family, name_frr, csr_ip, frr_ll, csr_ll, asn, artifacts, seconds):
    frr_show = f'show eigrp address-family {family} {asn} neighbors'
    cisco_show = f'show {"ip" if family == "ipv4" else "ipv6"} eigrp neighbors'
    peer_frr = '10.44.53.2' if family == 'ipv4' else csr_ll
    peer_cisco = '10.44.53.1' if family == 'ipv4' else frr_ll
    match = ipv4_peer if family == 'ipv4' else ipv6_peer
    deadline = time.monotonic() + seconds
    last_frr = last_cisco = ''
    while time.monotonic() < deadline:
        last_frr = frr_cli(name_frr, frr_show, artifacts)
        with Cisco(csr_ip, artifacts / 'cisco-cli.log') as router:
            last_cisco = router.run(cisco_show)
        if match(last_frr, peer_frr) and match(last_cisco, peer_cisco):
            return {'status': 'PASS', 'frr_peer': peer_frr, 'cisco_peer': peer_cisco}
        time.sleep(6)
    return {'status': 'FAIL', 'error': f'Missing neighbor: FRR expects {peer_frr}; Cisco expects {peer_cisco}',
            'frr_table': last_frr, 'cisco_table': last_cisco}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--scenario', required=True, type=pathlib.Path)
    parser.add_argument('--topology', required=True, type=pathlib.Path)
    parser.add_argument('--artifacts', required=True, type=pathlib.Path)
    a = parser.parse_args()
    a.artifacts.mkdir(parents=True, exist_ok=True)
    with a.scenario.open(encoding='utf8') as f: spec = yaml.safe_load(f)
    if spec.get('schema') != 'openeigrp-live-neighbor/v1' or spec.get('name') != '2R-neighbor':
        parser.error('unsupported live scenario schema or name')
    asn = spec.get('asn')
    if asn != 4473 or spec.get('process') != 'OpenEIGRP' or spec.get('families') != ['ipv4', 'ipv6']:
        parser.error('scenario does not match the implemented two-router fixture')
    report = {'scenario': spec['name'], 'timestamp_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
              'ipv4': {'status':'NOT_RUN'}, 'ipv6': {'status':'NOT_RUN'}}
    csr = 'clab-eigrp-neighbor-csr'
    frr = 'clab-eigrp-neighbor-oe'
    exit_status = 1
    try:
        topo = yaml.safe_load(a.topology.read_text(encoding='utf8'))
        if topo.get('name') != 'eigrp-neighbor' or set(topo.get('nodes', {})) != {'oe', 'csr'}:
            raise RuntimeError('Unexpected native netlab topology')
        for image in ['ghcr.io/diivious/openeigrp-frr:0.3.0', 'vrnetlab/vr-csr:17.03.06']:
            command(['docker', 'image', 'inspect', image], timeout=15)
        if not pathlib.Path('/usr/bin/sshpass').exists() and not shutil.which('sshpass'):
            raise RuntimeError('sshpass is required to connect to the CSR CLI')
        print('Deploying and configuring OpenEIGRP-FRR and CSR1000v via netlab...', flush=True)
        # 'netlab up' resolves the topology and custom templates from cwd.
        old_cwd = pathlib.Path.cwd()
        try:
            os.chdir(a.topology.parent)
            output = command(['netlab', 'up'], timeout=800, log_file=a.artifacts / 'deploy.log')
        finally:
            os.chdir(old_cwd)
        (a.artifacts / 'deploy.log').write_text(output, encoding='utf8')
        print('Configuring complete; checking OpenEIGRP daemon readiness...', flush=True)
        wait_frr(frr)
        print('Checking netlab-deployed OpenEIGRP named-mode configuration...', flush=True)
        frr_ll = validate_frr(frr, a.artifacts, spec['process'], asn)
        csr_ip = get_csr_mgmt_ip(csr)
        print('Checking CSR1000v IOS XE readiness...', flush=True)
        print('Cisco startup retries are logged to cisco-readiness.log', flush=True)
        csr_ll = wait_csr(csr_ip, a.artifacts, time.monotonic() + 420, spec['process'], asn)
        print(f'Link local addresses: FRR={frr_ll} CSR={csr_ll}', flush=True)
        for family in ('ipv4', 'ipv6'):
            print(f'Validating {family.upper()} EIGRP neighbor formation...', flush=True)
            try:
                report[family] = assess_family(family, frr, csr_ip, frr_ll, csr_ll,
                                               asn, a.artifacts, spec.get('neighbor_timeout', 120))
            except Exception as exc:
                report[family] = {'status':'FAIL', 'error':str(exc)}
            print(f"{report[family]['status']} {family.upper()} EIGRP neighbor interoperability", flush=True)
        exit_status = 0 if all(report[family]['status'] == 'PASS' for family in ('ipv4','ipv6')) else 1
    except Exception as exc:
        if 'netlab up' in str(exc) and not (a.artifacts / 'deploy.log').exists():
            (a.artifacts / 'deploy.log').write_text(str(exc), encoding='utf8')
        report['error'] = str(exc)
        print('FAIL setup/validation:', exc, file=sys.stderr, flush=True)
        exit_status = 1
    finally:
        for name in (frr, csr):
            try:
                output = command(['docker', 'logs', name], timeout=20, check=False)
                (a.artifacts / (name + '.log')).write_text(output, encoding='utf8')
            except Exception as exc:
                (a.artifacts / (name + '.log')).write_text(str(exc), encoding='utf8')
        (a.artifacts / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
        if exit_status:
            for family in ('ipv4','ipv6'):
                if report[family]['status'] == 'NOT_RUN':
                    print(f'NOT_RUN {family.upper()} (setup failed)', flush=True)
    return exit_status

if __name__ == '__main__':
    sys.exit(main())
