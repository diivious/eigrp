#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Live UUT parsing, native topology, shared configuration and readiness checks."""
import importlib.util
from pathlib import Path
from unittest import TestCase, mock
import tempfile
import unittest
from jinja2 import Environment, StrictUndefined
import yaml

ROOT = Path(__file__).resolve().parents[3]
NETLAB = ROOT / 'netlabs'
SCRIPT = NETLAB / 'uut' / '2R-neighbor.py'
spec = importlib.util.spec_from_file_location('live_neighbor', SCRIPT)
live = importlib.util.module_from_spec(spec)
spec.loader.exec_module(live)

class LiveNeighborTests(TestCase):
    def test_native_topology_and_named_mode(self):
        topo = yaml.safe_load((NETLAB / 'uut/2R-neighbor/topology.yml').read_text())
        self.assertEqual(topo['provider'], 'clab')
        self.assertEqual(len(topo['nodes']), 2)
        self.assertEqual(topo['nodes']['oe']['device'], 'frr')
        self.assertEqual(topo['nodes']['csr']['device'], 'csr')
        self.assertEqual(topo['nodes']['oe']['frr.daemons'], ['eigrpd'])
        self.assertEqual(topo['nodes']['oe']['config'], ['eigrp-named'])
        self.assertEqual(topo['nodes']['csr']['config'], ['eigrp-named'])
        self.assertIn('topology:../../config', topo['defaults']['paths.prepend.custom.dirs'])
        self.assertEqual(topo['defaults']['devices.frr.clab.group_vars.netlab_config_exec'], ['sleep 10'])
        self.assertEqual(len(topo['links']), 1)
        self.assertEqual(topo['links'][0]['oe']['ipv4'], '10.44.53.1/30')
        self.assertEqual(topo['links'][0]['csr']['ipv6'], '2001:db8:4473:1::2/64')
        scenario = yaml.safe_load((NETLAB / 'uut/2R-neighbor/scenario.yaml').read_text())
        self.assertEqual((scenario['process'], scenario['asn']), ('OpenEIGRP', 4473))

    def test_single_shared_template_renders_both_platforms(self):
        template = Environment(undefined=StrictUndefined, trim_blocks=True,
                               lstrip_blocks=True).from_string(
            (NETLAB / 'config/eigrp-named.j2').read_text())
        outputs = []
        for ifname, addr, rid in [('eth1', '10.44.53.1/30', '10.255.0.1/32'),
                                  ('GigabitEthernet2', '10.44.53.2/30', '10.255.0.2/32')]:
            rendered = template.render(loopback={'ipv4': rid}, interfaces=[
                {'ifname': ifname, 'type': 'p2p', 'ipv4': addr,
                 'ipv6': '2001:db8:4473:1::1/64'}])
            self.assertIn('router eigrp OpenEIGRP', rendered)
            self.assertIn('address-family ipv4 unicast autonomous-system 4473', rendered)
            self.assertIn('address-family ipv6 unicast autonomous-system 4473', rendered)
            self.assertIn(f'eigrp router-id {rid.split("/")[0]}', rendered)
            self.assertIn(f'network {addr.split("/")[0]} 0.0.0.0', rendered)
            self.assertEqual(rendered.count(f'af-interface {ifname}'), 2)
            self.assertNotIn('ipv6 router eigrp 4473', rendered)
            outputs.append(rendered)
        self.assertNotEqual(outputs[0], outputs[1])

    def test_ipv4_exact_and_negative(self):
        self.assertTrue(live.ipv4_peer(' 0 10.44.53.2 Gi2', '10.44.53.2'))
        self.assertFalse(live.ipv4_peer(' 0 10.44.53.22 Gi2', '10.44.53.2'))
        self.assertFalse(live.ipv4_peer('No neighbors', '10.44.53.2'))

    def test_ipv6_link_local_exact_and_negative(self):
        self.assertTrue(live.ipv6_peer('0 FE80::2 Gi2', 'fe80::2'))
        self.assertFalse(live.ipv6_peer('0 FE80::22 Gi2', 'fe80::2'))
        self.assertEqual(live.extract_link_local('fe80::aabb:ccff:fe01:1/64 scope link'),
                         'fe80::aabb:ccff:fe01:1')

    def test_rejected_cli_is_failure(self):
        for error in ('% Invalid input detected at ^ marker', 'unknown command',
                      '% Ambiguous command', '% Incomplete command'):
            with self.assertRaises(RuntimeError):
                live.assert_cli(error, 'test')

    def test_bidirectional_ipv4_pass_requires_both_peers(self):
        with tempfile.TemporaryDirectory() as path:
            class MockCisco:
                def __init__(self, *args): pass
                def __enter__(self): return self
                def __exit__(self, *args): pass
                def run(self, *args): return '0 10.44.53.1 Gi2'
            with mock.patch.object(live, 'frr_cli', return_value='10.44.53.2 Up'), \
                 mock.patch.object(live, 'Cisco', MockCisco):
                result = live.assess_family('ipv4','frr','csr','fe80::1','fe80::2',4473,Path(path),1)
                self.assertEqual(result['status'], 'PASS')

    def test_ipv6_missing_cisco_peer_fails(self):
        with tempfile.TemporaryDirectory() as path:
            class MockCisco:
                def __init__(self, *args): pass
                def __enter__(self): return self
                def __exit__(self, *args): pass
                def run(self, *args): return 'No neighbors'
            with mock.patch.object(live, 'frr_cli', return_value='FE80::2 Up'), \
                 mock.patch.object(live, 'Cisco', MockCisco), \
                 mock.patch.object(live.time, 'sleep', return_value=None):
                result = live.assess_family('ipv6','frr','csr','fe80::1','fe80::2',4473,Path(path),0.01)
                self.assertEqual(result['status'], 'FAIL')

    def test_frr_readiness_retries_until_eigrpd_accepts_cli(self):
        outputs = [RuntimeError('failed to connect'), 'FRRouting 10.8.0-dev']
        with mock.patch.object(live, 'command', side_effect=outputs) as run, \
             mock.patch.object(live.time, 'sleep') as sleep:
            live.wait_frr('clab-eigrp-neighbor-oe', seconds=30)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args.args[0],
                         ['docker','exec','clab-eigrp-neighbor-oe',
                          'vtysh','-d','eigrpd','-c','show version'])
        sleep.assert_called_once()

    def test_frr_readiness_timeout(self):
        with mock.patch.object(live, 'command', side_effect=RuntimeError('failed to connect')), \
             mock.patch.object(live.time, 'sleep', return_value=None):
            with self.assertRaisesRegex(TimeoutError, 'failed to connect'):
                live.wait_frr('frr', seconds=0.01)

    def test_frr_config_validation_read_only(self):
        outputs = []
        def fake_command(argv, **kwargs):
            outputs.append(argv)
            return '3: eth1 inet6 fe80::1/64 scope link'
        with tempfile.TemporaryDirectory() as path:
            with mock.patch.object(live, 'frr_cli', return_value=(
                    'router eigrp OpenEIGRP\n'
                    'address-family ipv4 unicast autonomous-system 4473\n'
                    'address-family ipv6 unicast autonomous-system 4473\n')), \
                 mock.patch.object(live, 'command', side_effect=fake_command):
                self.assertEqual(live.validate_frr('frr',Path(path),'OpenEIGRP',4473), 'fe80::1')
        self.assertFalse(any('configure terminal' in a for a in outputs))

    def test_cisco_readiness_quiet_retries_and_diagnostics(self):
        class MockCisco:
            attempts = 0
            def __init__(self, *args): pass
            def __enter__(self):
                MockCisco.attempts += 1
                if MockCisco.attempts < 3:
                    raise RuntimeError('SSH banner not ready')
                return self
            def __exit__(self, *args): pass
            def run(self, command):
                if command == 'show running-config':
                    return ('router eigrp OpenEIGRP\n'
                            'address-family ipv4 unicast autonomous-system 4473\n'
                            'address-family ipv6 unicast autonomous-system 4473\n')
                return 'link local address: FE80::2'
        import io
        from contextlib import redirect_stdout
        with tempfile.TemporaryDirectory() as path:
            output=io.StringIO()
            with mock.patch.object(live, 'Cisco', MockCisco), \
                 mock.patch.object(live.time, 'sleep', return_value=None), \
                 redirect_stdout(output):
                self.assertEqual(live.wait_csr('csr',Path(path),live.time.monotonic()+5,'OpenEIGRP',4473),
                                 'fe80::2')
            self.assertIn('Cisco ready after 3 connection attempts',output.getvalue())
            self.assertNotIn('SSH banner not ready',output.getvalue())
            self.assertEqual((Path(path)/'cisco-readiness.log').read_text().count('SSH banner not ready'),2)

    def test_cisco_configuration_missing_is_fatal_not_420_second_retry(self):
        class MockCisco:
            def __init__(self, *args): pass
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def run(self, cmd): return 'hostname CSR1\nrouter eigrp OTHER\n'
        with tempfile.TemporaryDirectory() as path:
            with mock.patch.object(live, 'Cisco', MockCisco), \
                 mock.patch.object(live.time, 'sleep') as sleep:
                with self.assertRaisesRegex(RuntimeError, 'named-mode configuration missing'):
                    live.wait_csr('csr',Path(path),live.time.monotonic()+420,'OpenEIGRP',4473)
                sleep.assert_not_called()

    def test_deployment_calls_native_netlab_and_never_configures_protocol_directly(self):
        seen = []
        def fake_command(argv, **kwargs):
            seen.append(argv)
            if argv[:3] == ['docker', 'image', 'inspect']:
                return 'image exists'
            if argv == ['netlab', 'up']:
                return 'deployed'
            if argv[:2] == ['docker', 'logs']:
                return 'container log'
            raise AssertionError(f'Unexpected command: {argv}')
        with tempfile.TemporaryDirectory() as path:
            args = ['2R-neighbor.py','--scenario',str(NETLAB/'uut/2R-neighbor/scenario.yaml'),
                    '--topology',str(NETLAB/'uut/2R-neighbor/topology.yml'),
                    '--artifacts',path]
            with mock.patch.object(live, 'command', side_effect=fake_command), \
                 mock.patch.object(live, 'wait_frr'), \
                 mock.patch.object(live, 'validate_frr', return_value='fe80::1'), \
                 mock.patch.object(live, 'get_csr_mgmt_ip', return_value='172.20.20.3'), \
                 mock.patch.object(live, 'wait_csr', return_value='fe80::2'), \
                 mock.patch.object(live, 'assess_family', return_value={'status':'PASS'}), \
                 mock.patch.object(live.shutil, 'which', return_value='/usr/bin/sshpass'), \
                 mock.patch('sys.argv',args):
                self.assertEqual(live.main(),0)
        self.assertIn(['netlab','up'],seen)
        self.assertFalse(any(a[:2]==['containerlab','deploy'] for a in seen))
        self.assertFalse(any('configure terminal' in a for a in seen))

if __name__ == '__main__':
    unittest.main()
