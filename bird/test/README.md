# BIRD EIGRP Tests

This directory owns BIRD-specific EIGRP adapter, integration, and UUT material.
Host-independent tests remain under `test/portable/` or `test/common/`;
FRR-native tests remain under `frr/test/`.

Add BIRD-native build, integration, and runtime tests here as the BIRD adapter
coverage is implemented. Do not reuse FRR test objects, FRR lifecycle
assumptions, or operating-system-specific abstractions as the BIRD integration
contract.
