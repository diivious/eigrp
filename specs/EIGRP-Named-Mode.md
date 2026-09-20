# Configure EIGRP Named Mode

> **Reference role:** This file summarizes Cisco named-mode documentation. It is not the project implementation specification or protocol authority. Project CLI requirements are defined by `cli-spec.md`; architecture is defined by `design-spec.md`. Cisco-only migration/platform features mentioned here do not automatically become project requirements.

## Introduction

This document describes the named Enhanced Interior Gateway Routing Protocol (EIGRP) mode feature and differences between traditional and named mode.

## Prerequisites

### Requirements
Cisco recommends that you have basic knowledge of IP Routing and the EIGRP protocol.

### Components Used
This document is not restricted to specific software and hardware versions.

The information in this document was created from the devices in a specific lab environment. All of the devices used in this document started with a cleared (default) configuration. If your network is live, ensure that you understand the potential impact of any command.

## Background Information

The traditional way to configure EIGRP requires various parameters to be configured under the interface and EIGRP configuration mode.

In order to configure EIGRP IPv4 and IPv6, it is required to configure separate EIGRP instances.

Traditional EIGRP does not support Virtual Routing and Forwarding (VRF) in IPv6 EIGRP implementations.

With Named mode EIGRP, everything is configured at a single place under the EIGRP configuration and there are no restrictions as mentioned previously.

## Configure

### Network Diagram

Sample topology: R1 (10.1.1.1) connected to R2 (10.1.1.2).

Unlike the traditional method, the EIGRP instance is neither created nor started when this is configured on the router:

```bash
R1(config)# router eigrp TEST
```

The instance is created when address-family and autonomous system number is configured:

```bash
R1(config-router)# address-family ipv4 unicast autonomous-system 1
```

With this named mode, only a single instance of EIGRP needs to be created. It can be used for all address family types.

It also supports multiple VRFs limited only by available system resources.

**Note:** In the named mode, configuration of the address-family does not enable IPv4 routing as a traditional configuration of IPv4 EIGRP. A `no shut` is required to start the process:

```bash
router eigrp [virtual-instance-name | asystem] [no] shutdown
```

### Named EIGRP Modes

Named EIGRP has three modes:

- **address-family configuration mode** — `(config-router-af)#`
- **address-family interface configuration mode** — `(config-router-af-interface)#`
- **address-family topology configuration mode** — `(config-router-af-topology)#`

#### Address-family Configuration Mode

Enter with:

```bash
R1(config-router)# address-family ipv4 unicast autonomous-system 1
R1(config-router-af)# ?
```

Key commands: `af-interface`, `eigrp`, `network`, `neighbor`, `eigrp router-id`, `topology`, `shutdown`, `timers`, etc.

Configures: Networks, EIGRP neighbor, EIGRP Router-id.

**Traditional Configuration Example**

```bash
interface GigabitEthernet 0/0
 ip bandwidth-percent eigrp 1 75
 ipv6 enable
 ipv6 eigrp 1
 no shut
!
router eigrp 1
 eigrp router-id 10.10.10.1
 network 0.0.0.0 0.0.0.0
!
ipv6 router eigrp 1
 eigrp router-id 10.10.10.1
 no shut
```

**Named Configuration Example**

```bash
router eigrp TEST
!
address-family ipv4 unicast autonomous-system 1
 network 0.0.0.0
 eigrp router-id 10.10.10.1
 no shutdown
 exit-address-family
!
address-family ipv6 unicast autonomous-system 1
 eigrp router-id 10.10.10.1
 no shutdown
 exit-address-family
```

#### Address-family Interface Configuration Mode

Interface-specific commands (authentication, split-horizon, summary-address, hello-interval, hold-time, passive-interface, etc.) moved here from physical interface config.

Enter with:

```bash
R1(config-router-af)# af-interface g0/0
R1(config-router-af-interface)# ?
```

Use `af-interface default` to apply to all interfaces.

#### Address-family Topology Configuration Mode

For topology table options: redistribution, distance, offset-list, variance, maximum-paths, summary-metric, etc.

Enter with:

```bash
R1(config-router-af)# topology base
R1(config-router-af-topology)# ?
```

## Comparison

[Image: Side-by-side comparison of Traditional EIGRP vs Named EIGRP configuration (includes VRF/IPv6 support notes)]

## Availability

EIGRP named configuration available from:

- Cisco IOS 15.0(1)M
- 12.2(33)SRE
- 12.2(33)XNE
- Cisco IOS XE Release 2.5

## Automatic Conversion to Named EIGRP — Cisco reference only

> `eigrp upgrade-cli` is outside this project's EIGRP CLI scope. The syntax below is retained only as Cisco reference material.

Cisco documents `eigrp upgrade-cli <name>` inside an EIGRP process to convert without impacting peering.

**Example Conversion**

Traditional:

```bash
router eigrp 1
 network 10.10.10.1 0.0.0.0
!
interface Ethernet0/0
 ip address 10.10.10.1 255.255.255.0
 ip hello-interval eigrp 1 100
```

Run:

```bash
R1(config)# router eigrp 1
R1(config-router)# eigrp upgrade-cli TEST
```

(Confirm yes)

**Resulting Named Config:**

```bash
router eigrp TEST
!
address-family ipv4 unicast autonomous-system 1
!
af-interface Ethernet0/0
 hello-interval 100
 exit-af-interface
!
topology base
 exit-af-topology
 network 10.10.10.1 0.0.0.0
 exit-address-family
```

## Verify

No specific verification procedure available.

## Troubleshoot

No specific troubleshooting information available.
