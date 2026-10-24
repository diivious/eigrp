# OpenEIGRP netlabs

## Getting started

`netlabs/` provides two independent tools for a shared Linux lab appliance:

- **`netlabs/tools/remote-lab.sh`** deploys a numbered instructional lab and
  leaves it running for an operator to follow the lab guide. No numbered labs
  are deployable yet; the curriculum below describes planned labs.
- **`netlabs/tools/remote-uut.sh`** deploys an automated interoperability test,
  validates it, collects reports, and tears down the topology. The implemented
  `2R-neighbor` scenario tests OpenEIGRP-FRR 0.3.0 against Cisco CSR1000v
  17.03.06 for bidirectional IPv4 and IPv6 EIGRP neighbor formation.

The numeric prefix in `2R-neighbor` identifies the **total number of routers**
regardless of their implementations. These live tests are separate from the
portable `make uut` framework in `eigrp/test/uut/`.

### Prerequisites

The **execution host** must be Linux with Docker, Containerlab, netlab,
Ansible, KVM acceleration for Cisco virtual routers, and the dedicated `netlab`
account. The provisioner used for StormTrooper installs these dependencies and
creates `/home/netlab/netlab-venv/`, `/home/netlab-data/runs/`, and
`/home/netlab-data/results/`. Before using these scripts, verify that the
`netlab` account can run `netlab test clab` and access Docker images.

The `2R-neighbor` scenario also requires these exact image tags on the execution
host: `ghcr.io/diivious/openeigrp-frr:0.3.0` and
`vrnetlab/vr-csr:17.03.06`. CSR1000v IOS XE takes several minutes to boot.
The runner requires `sshpass` on the execution host for its Cisco CLI checks.

The **client** can be macOS or Linux and needs Bash, SSH, and `tar`. Python,
Docker, Containerlab, and netlab are not required on the client. If using
password-based SSH instead of public keys, install `sshpass` on the client too.
The client needs a local OpenEIGRP repository checkout containing `netlabs/`.

### Configure the execution host

Edit **`netlabs/remote-lab.cfg`** in your local checkout (not `netlabs.cfg`):

```bash
REMOTE_HOST=204.75.212.13
REMOTE_USER=netlab
REMOTE_PASSWORD=netlab
REMOTE_SSH_PORT=22
```

`REMOTE_HOST` can be a resolvable hostname or an IPv4 address. `netlab` is the
requested default for both user and password; the account password must be set
separately if you intend to use password authentication. **SSH keys are
preferred.** In that case, the password setting is unused. Treat the config as
a trusted Bash file because the clients source it directly.

Do **not commit a real password** to Git. Use an untracked credential file
outside your checkout if you need one:

```bash
chmod 600 /path/to/private-remote-lab.cfg
export REMOTE_LAB_CONFIG=/path/to/private-remote-lab.cfg
```

The override file must specify the same variable names. A password is not
needed when SSH public-key authentication works.

Before your first run, verify unattended SSH authentication from your client:

```bash
ssh -o BatchMode=yes netlab@204.75.212.13 whoami
# expected: netlab
```

If this fails but interactive password SSH succeeds, authorize your client's
public key in `/home/netlab/.ssh/authorized_keys` on the Linux execution host.
The host does **not** need a new SSH key pair. Use the actual configured
`REMOTE_HOST` instead of the example address above.

### Can I use localhost?

**Yes, with an important restriction:** `REMOTE_HOST=127.0.0.1` (or
`REMOTE_HOST=localhost`) works when the client script runs **on the Linux
execution host itself**, provided it has the same provisioning, Docker/KVM
images, `netlab` account, running SSH server, and SSH authentication to
`netlab@127.0.0.1`. The scripts still use SSH, even for loopback. Confirm with:

```bash
ssh -o BatchMode=yes netlab@127.0.0.1 whoami
# expected: netlab
```

Setting `127.0.0.1` **on macOS points to the Mac**, not StormTrooper. It does
not enable running the CSR1000v KVM lab natively on macOS. Likewise, an
unprovisioned local Linux machine is not automatically a lab appliance.

### Run tests and manage labs

From the OpenEIGRP repository root:

```bash
netlabs/tools/remote-uut.sh --list
netlabs/tools/remote-uut.sh --scenario 2R-neighbor

netlabs/tools/remote-lab.sh --status
netlabs/tools/remote-lab.sh --lab 101  # when lab 101 is implemented
netlabs/tools/remote-lab.sh --connect  # choose a router
netlabs/tools/remote-lab.sh --stop     # destroy the active lab
```

Only **one lab or UUT session** can occupy the execution host at a time. The
server maintains authoritative session state in `/home/netlab/.remote-lab/`;
interactive labs persist until `--stop`, while UUTs tear down automatically.
The tools stream progress and return a final PASS/FAIL. A failure to clean up
retains the session for recovery rather than claiming success.

UUT reports are copied to `netlabs/results/uut-.../` in the client checkout;
`report.json` contains separate IPv4/IPv6 verdicts, `run-status.txt` records
execution and cleanup status, and the logs include deployment, CLI, and router
startup diagnostics. Transient Cisco SSH retries and macOS tar extended
attribute warnings are suppressed or retained in diagnostic logs when possible.
A successful neighbor test does **not** verify route exchange or convergence.

### Code layout and developer checks

```text
netlabs/
├── README.md                 # Operation guide and instructional curriculum
├── remote-lab.cfg            # Client SSH settings; keep secrets out of Git
├── config/eigrp-named.j2    # Shared named-mode Cisco/OpenEIGRP configuration
├── tools/
│   ├── remote-lab.sh
│   ├── remote-uut.sh
│   └── remote/{common.sh,runner.sh}
├── uut/
│   ├── 2R-neighbor.py       # Live test driver
│   ├── 2R-neighbor/         # Native netlab topology and scenario
│   └── tests/
├── labs/                    # Numbered instructional labs (not yet deployed)
├── tests/                   # Remote runner regression tests
└── results/                 # Generated artifacts; excluded from Git
```

```bash
python3 -m unittest discover -s netlabs/uut/tests -p 'test_2r_neighbor.py'
netlabs/tests/test_remote_runtime.sh
```

These regression tests use mocked external tools. Live interoperability and
cleanup must be verified separately on the provisioned execution host.

---

**Platform:** [netlab](https://netlab.tools/) for network labs; portable OpenEIGRP tools and the project repository for developer labs

**Scope:** 100-, 200-, 300-, and 400-level courses; instructional design proposal, not a claim that all scenarios are implemented or verified
**Purpose:** A progressive training path for routing practitioners and contributors, with independent lab entry points and measurable practical outcomes.

## Curriculum rules

1. **Cumulative knowledge, independent execution.** A prerequisite names concepts the student should understand, never files or runtime state that must be carried forward from an earlier lab. Every exercise deploys its own complete baseline.
2. **One clear new skill per exercise.** Earlier topics can appear as preconfigured context but should not require repetitive setup unless doing so is the learning objective.
3. **Observable completion criteria.** Success requires correct neighbor, topology, control-plane, host RIB/FIB, packet, convergence or development-test evidence, as appropriate.
4. **Platform-neutral learning intent.** Network topology, requirements and expectations belong under `netlabs/common/`. Platform-specific configuration, lifecycle and state collection belong in separate integrations.
5. **Teach, then assess.** Each student README explains concepts and commands without giving away the reference solution. A separate instructor solution covers reasoning, expected observations and restoration.
6. **No silent passes.** Features and validation targets unavailable in a given integration must be marked unqualified; no lab can claim success on accepted CLI/config text alone.
7. **Preserve reproducibility.** Pin image/tool versions for published reference runs, specify resource requirements and provide teardown/reset commands.
8. **Developer labs use real implementation behavior.** The 400 level retains the project's branch authority, test requirements, and read-only GitHub workflow; it is not a substitute for portable UUT qualifications.

## Degree-style course map

| Course | Title | Learner | Entry knowledge | Labs |
|---|---|---|---|---|
| OEIGRP 100 | EIGRP Foundations | Entry CCNP / experienced network operators | IPv4 and basic router CLI | 101–107 |
| OEIGRP 200 | Routing Policy and Operations | Advanced CCNP | 100-level concepts | 201–212 |
| OEIGRP 300 | Advanced Design and Troubleshooting | CCIE-style practitioners | 200-level concepts | 301–310 |
| OEIGRP 400 | Developing with OpenEIGRP | Contributors and maintainers | C/Python/Git, EIGRP, testing | 401–410 |

**Difficulty labels** describe task complexity, not official certification mapping. The course numbers are pedagogical organization and do not indicate accreditation or university credit.

## Standard lab deliverables

Each network lab should include `topology.yml`, a detailed student `README.md`, automated validation, platform-specific starting/reference configuration, and a separately discoverable instructor solution. The README shall document: practical scenario and audience; learning objectives; prerequisite *knowledge*; topology and addressing diagram; equipment and runtime requirements; baseline configuration and intentionally omitted parts; student tasks and restrictions; verification commands and expected observations; explanation of new concepts; diagnosis/extension exercise; cleanup; and references (including relevant RFC 7868 material).

Each developer lab should add build/test commands, an exercise branch or reproducible fixture, affected APIs/modules, regression evidence required, and explicit expected artifacts. Do not print real credentials or embed proprietary Cisco configuration outputs as redistributed teaching material.

**Release gate:** A lab is *planned* until its environment, commands, reference solution and semantic validations are executed and confirmed on a named OpenEIGRP integration and image version.

## Repository layout and lab status

The operational layout and commands are documented in **Getting started**
above. The instructional curriculum below remains a specification: a lab is
not deployable until its numbered `netlabs/labs/NNN/` directory includes a
working `topology.yml` and its learning materials. `netlabs/uut/2R-neighbor`
is an implemented *automated interoperability test*, not lab 101.

All netlab-specific tooling, templates, labs, tests, and configuration belong
under `netlabs/`, including future 400-level lab materials.

## OEIGRP 100: EIGRP Foundations

**Level:** CCNP foundation
**Course outcome:** Understand adjacency formation, route propagation, metrics, and basic recovery before manipulating policy.
**Entry:** A Linux/containerlab environment capable of launching the selected OpenEIGRP integration. Basic IPv4 networking, subnets, and routing tables.
**Labs:** 7

| Lab | Title | Difficulty | Primary new concept |
|---|---|---|---|
| 101 | First EIGRP Network | Introductory | EIGRP AS membership; HELLO/adjacency; initial UPDATE exchange |
| 102 | Interface Selection and Passive Interfaces | Introductory | Interface activation versus advertisement; passive interfaces |
| 103 | Explicit Unicast Neighbors | Intermediate | Explicit neighbor relationships; unicast neighbor configuration; multicast behavior |
| 104 | Composite Metrics and Path Preference | Intermediate | EIGRP vector/composite metrics; K values; administrative versus EIGRP metric |
| 105 | Reading EIGRP Topology | Intermediate | Topology entries; reported distance; feasible distance; successor versus alternative path |
| 106 | Equal-Cost Multipath | Intermediate | ECMP eligibility; maximum-paths; forwarding-table installation |
| 107 | Basic Failure and Recovery | Intermediate | Adjacency loss; route withdrawal; reconvergence and recovery |

### Lab 101: First EIGRP Network

- **Difficulty:** Introductory
- **Prerequisite knowledge:** IPv4 addressing and static versus dynamic routing.
- **New concepts:** EIGRP AS membership; HELLO/adjacency; initial UPDATE exchange.
- **Learning objectives / student work:** Configure one EIGRP autonomous system on two directly connected routers; identify the neighbor and a learned prefix.
- **Starting configuration:** Two addressed routers, links up, EIGRP not configured; separate attached test prefixes.
- **Expected observable outcome:** Both adjacencies reach Up; expected remote prefixes appear in topology and RIB; explain the difference between connected and EIGRP-learned routes.

### Lab 102: Interface Selection and Passive Interfaces

- **Difficulty:** Introductory
- **Prerequisite knowledge:** 101: neighbor formation and network selection.
- **New concepts:** Interface activation versus advertisement; passive interfaces.
- **Learning objectives / student work:** Determine which interfaces participate in EIGRP; enable advertisements from a passive-facing LAN without forming an unintended adjacency.
- **Starting configuration:** Three routers with working AS on transit links and isolated LAN/loopback interfaces; per-interface routing policy is left for the student.
- **Expected observable outcome:** All intended LAN prefixes propagate, transit neighbors remain Up, and no neighbor forms on passive-facing interfaces.

### Lab 103: Explicit Unicast Neighbors

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 101–102: neighbors and interface membership.
- **New concepts:** Explicit neighbor relationships; unicast neighbor configuration; multicast behavior.
- **Learning objectives / student work:** Configure explicit neighbors on a multiaccess segment and distinguish unicast EIGRP traffic from normal multicast discovery.
- **Starting configuration:** Three routers on a common broadcast segment, with addressing and EIGRP AS configured; explicit-neighbor policy absent.
- **Expected observable outcome:** Specified adjacencies form, unrequested adjacencies do not, and packet evidence supports the observed unicast behavior.

### Lab 104: Composite Metrics and Path Preference

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 101–102: basic routes; bandwidth and delay concepts.
- **New concepts:** EIGRP vector/composite metrics; K values; administrative versus EIGRP metric.
- **Learning objectives / student work:** Predict preferred routes from path characteristics; change supported metric inputs and explain a resulting next-hop change.
- **Starting configuration:** Three-router network with two viable paths, stable adjacencies, and unequal interface metrics; all routing enabled.
- **Expected observable outcome:** Student computes or explains metric ordering, sees the expected path in topology and RIB, and restores a known baseline.

### Lab 105: Reading EIGRP Topology

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 104: basic metric comparison.
- **New concepts:** Topology entries; reported distance; feasible distance; successor versus alternative path.
- **Learning objectives / student work:** Interpret topology output and correlate per-neighbor paths with the installed route; explain why not every known path is installed.
- **Starting configuration:** Four-router diamond, stable EIGRP and deterministic link metrics; no configuration change is initially required.
- **Expected observable outcome:** Identify every learned path and the installed successor, record RD/FD, and distinguish topology visibility from kernel-route installation.

### Lab 106: Equal-Cost Multipath

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 104–105: metrics, topology and RIB.
- **New concepts:** ECMP eligibility; maximum-paths; forwarding-table installation.
- **Learning objectives / student work:** Produce two equal-cost routes to one destination and verify both protocol selection and actual forwarding entries.
- **Starting configuration:** Four-router diamond with equalized paths and a reachable destination; EIGRP operational; multipath controls exposed to the student.
- **Expected observable outcome:** Both equal-cost next hops are selected and, when supported by the adapter, installed in the operating-system RIB/FIB.

### Lab 107: Basic Failure and Recovery

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 101–106: neighbor, route and topology inspection.
- **New concepts:** Adjacency loss; route withdrawal; reconvergence and recovery.
- **Learning objectives / student work:** Disable a link, observe route withdrawal and alternate path use, then restore service and validate a stable network.
- **Starting configuration:** Four-router ring with fully functioning adjacency and reachable remote networks; failure-injection controls available.
- **Expected observable outcome:** Traffic and route selection follow expected paths before, during and after failure; no persistent stale route remains.

## OEIGRP 200: EIGRP Routing Policy and Operations

**Level:** Advanced CCNP
**Course outcome:** Control advertisements, boundaries, traffic preference, security and operational limits.
**Entry:** 100-level concepts or equivalent experience. Every lab starts its own preconfigured topology.
**Labs:** 12

| Lab | Title | Difficulty | Primary new concept |
|---|---|---|---|
| 201 | Default Route Distribution | Intermediate | Default route origin, advertisement scope and route preference |
| 202 | Prefix Lists and Distribute Lists | Intermediate | Prefix matches, masks, ge/le bounds and direction-specific filtering |
| 203 | ACLs and Route Maps | Intermediate | ACL-based policy; route-map match/permit/deny evaluation |
| 204 | Offset Lists and Metric Steering | Advanced | Inbound/outbound metric offsets and traffic preference without physical-link changes |
| 205 | Route Summarization | Intermediate | EIGRP summarization; component suppression; aggregate advertisements |
| 206 | Selective Route Leaking | Advanced | Selective more-specific advertisements through a summary; longest-prefix forwarding |
| 207 | Redistributing Connected and Static Routes | Intermediate | Route origination classes; redistribution seed metric; external route attributes |
| 208 | Multiple Autonomous Systems | Advanced | Distinct EIGRP AS boundaries; mutual redistribution; feedback prevention |
| 209 | Redistributed Route Next Hops | Advanced | Next-hop-self behavior; third-party next hops and shared-medium forwarding |
| 210 | External Route Filtering and Tags | Advanced | EIGRP external route metadata; tags; policy at redistribution boundaries |
| 211 | Operational Route Limits | Advanced | Maximum hop count; maximum-prefix thresholds; warning and enforcement behavior |
| 212 | MD5 Authentication and Key Rollover | Advanced | MD5 keychains, matching keys, key identity and time-based transitions |

### Lab 201: Default Route Distribution

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 107: successful route propagation and recovery.
- **New concepts:** Default route origin, advertisement scope and route preference.
- **Learning objectives / student work:** Provide default connectivity to selected branches while retaining specific routes where required; verify behavior on primary-path failure.
- **Starting configuration:** Hub-and-spoke topology with AS and branch LANs operating; default origin and advertisement policy not configured.
- **Expected observable outcome:** Only intended routers learn a usable default; forwarding succeeds and fallback behavior matches the task.

### Lab 202: Prefix Lists and Distribute Lists

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 102, 105: advertisement and route inspection.
- **New concepts:** Prefix matches, masks, ge/le bounds and direction-specific filtering.
- **Learning objectives / student work:** Construct inbound/outbound prefix filters; explain which routes disappear and which are retained.
- **Starting configuration:** Three-router chain with diverse advertised prefix lengths and no filtering; learner receives an explicit permitted-prefix policy.
- **Expected observable outcome:** Accepted prefixes remain reachable, rejected prefixes are absent from the proper recipient, and unrelated routes remain unaffected.

### Lab 203: ACLs and Route Maps

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 202: prefix-based filtering.
- **New concepts:** ACL-based policy; route-map match/permit/deny evaluation.
- **Learning objectives / student work:** Implement equivalent filtering through ACL and route-map techniques; explain important matching differences.
- **Starting configuration:** Three-router route-policy lab with loopback prefixes and pre-established adjacencies; policy objects absent.
- **Expected observable outcome:** Target prefix sets meet the specification using both approaches, including negative cases and implicit deny behavior.

### Lab 204: Offset Lists and Metric Steering

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 104: metric calculation; 202: policy scope.
- **New concepts:** Inbound/outbound metric offsets and traffic preference without physical-link changes.
- **Learning objectives / student work:** Make a backup link less preferred while maintaining its adjacency; prove the link is used after preferred-path loss.
- **Starting configuration:** Three-router triangle with two stable paths and defined initial costs; physical interface bandwidth and delay fixed.
- **Expected observable outcome:** Primary path matches the requested policy, backup carries routes after failure, and post-recovery preference is restored.

### Lab 205: Route Summarization

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 105: topology inspection; 202: prefix handling.
- **New concepts:** EIGRP summarization; component suppression; aggregate advertisements.
- **Learning objectives / student work:** Aggregate several contiguous networks at an edge while preserving reachability and checking component-route visibility.
- **Starting configuration:** Four routers; edge advertises four contiguous LAN prefixes; no summary configured.
- **Expected observable outcome:** Remote routers see the intended aggregate rather than component prefixes; forwarding and failure behavior remain correct.

### Lab 206: Selective Route Leaking

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 205: summaries; 203: route maps.
- **New concepts:** Selective more-specific advertisements through a summary; longest-prefix forwarding.
- **Learning objectives / student work:** Expose only selected more-specific networks through a summary and direct traffic over the intended edge.
- **Starting configuration:** Four-router network with two summary-capable edges; summaries configured and stable; leak policy missing.
- **Expected observable outcome:** Only selected more-specific routes appear; other components remain summarized; next hops follow longest-prefix expectations.

### Lab 207: Redistributing Connected and Static Routes

- **Difficulty:** Intermediate
- **Prerequisite knowledge:** 101–105: learned-versus-source routes; 203: policy basics.
- **New concepts:** Route origination classes; redistribution seed metric; external route attributes.
- **Learning objectives / student work:** Redistribute designated connected/static prefixes into EIGRP without unintentionally advertising other host routes.
- **Starting configuration:** Three routers with EIGRP running and source connected/static routes present only in the origin router RIB.
- **Expected observable outcome:** Selected external destinations propagate and install; source routes and advertisements are withdrawn independently as configured.

### Lab 208: Multiple Autonomous Systems

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 207: redistribution and external routes.
- **New concepts:** Distinct EIGRP AS boundaries; mutual redistribution; feedback prevention.
- **Learning objectives / student work:** Connect two EIGRP domains through a border router and verify selective bidirectional reachability.
- **Starting configuration:** Four routers in two functioning AS domains; border device has interfaces and both AS instances but no redistribution policy.
- **Expected observable outcome:** Expected inter-domain prefixes propagate, unintended prefixes do not, and boundaries do not create persistent route feedback.

### Lab 209: Redistributed Route Next Hops

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 208: two-AS routing; 105: next-hop selection.
- **New concepts:** Next-hop-self behavior; third-party next hops and shared-medium forwarding.
- **Learning objectives / student work:** Inspect next hops for redistributed destinations and apply supported next-hop policy where the topology requires it.
- **Starting configuration:** Multiaccess network with two AS instances and redistribution already functioning; default next-hop handling unchanged.
- **Expected observable outcome:** Resolved next-hop addresses are reachable and forwarding follows the required path; the student explains when preserving a third-party hop is safe.

### Lab 210: External Route Filtering and Tags

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 203 and 207–208: route maps and external routes.
- **New concepts:** EIGRP external route metadata; tags; policy at redistribution boundaries.
- **Learning objectives / student work:** Apply route tags and selective filters to limit external route propagation and verify withdrawal on policy changes.
- **Starting configuration:** Four-router, two-AS setup with several redistributed networks and known initial tag values; final policy incomplete.
- **Expected observable outcome:** Permitted external routes remain, denied routes are not installed, and route tag origin is visible through supported operational data.

### Lab 211: Operational Route Limits

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 105 and 207: topology size and redistribution.
- **New concepts:** Maximum hop count; maximum-prefix thresholds; warning and enforcement behavior.
- **Learning objectives / student work:** Configure operating limits; cross a threshold using supplied prefix fixtures; compare warning and enforcement outcomes.
- **Starting configuration:** Parameterized multi-router/prefix-source lab with controllable hop chain and route volume; basic routing converged.
- **Expected observable outcome:** Observed warnings and admitted/rejected route counts match the configured policy; boundary and recovery cases pass.

### Lab 212: MD5 Authentication and Key Rollover

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 103: neighbor establishment; 107: reconvergence.
- **New concepts:** MD5 keychains, matching keys, key identity and time-based transitions.
- **Learning objectives / student work:** Secure adjacencies, demonstrate mismatched-key isolation, and perform an overlapping-key rollover where supported.
- **Starting configuration:** Three-router LAN with working EIGRP and test-only keychains; authentication policy initially absent.
- **Expected observable outcome:** Only correctly keyed peers form adjacencies; valid key rollover preserves or restores service within specified timing; failed keys never create false success.

## OEIGRP 300: Advanced EIGRP Design and Troubleshooting

**Level:** CCIE-style applied routing
**Course outcome:** Explain and diagnose complex EIGRP decisions, failure chains, inter-domain behavior and IPv6 scenarios.
**Entry:** 200-level operational and policy concepts or equivalent expertise; every challenge includes its own fully configured prerequisites.
**Labs:** 10

| Lab | Title | Difficulty | Primary new concept |
|---|---|---|---|
| 301 | The Feasibility Condition | Advanced | Strict feasibility condition; feasible successor selection |
| 302 | Variance and Unequal-Cost Routing | Advanced | Variance admission; unequal-cost installed paths; feasibility versus cost threshold |
| 303 | DUAL Diffusing Computation | Expert | ACTIVE/PASSIVE transitions; QUERY and REPLY dependencies |
| 304 | SIA and Fault Isolation | Expert | Stuck-in-active detection; delayed/missing REPLY; failure scoping |
| 305 | Dual Failures and Query Scope | Expert | Sequential/simultaneous failure interactions; bounded query domain |
| 306 | Named Mode for IPv4 | Advanced | Named EIGRP hierarchy; address-family, AF-interface and topology configuration |
| 307 | Named Mode for IPv6 | Advanced | IPv6 AF operation; link-local peers and next hops; IPv6 prefix advertisements |
| 308 | Named Authentication and Recovery | Expert | Authentication configuration in named mode; failure and recovery across AFs |
| 309 | IPv6 Next Hop Across AS Boundaries | Expert | IPv6 redistribution attribution; next-hop reachability on multiaccess links |
| 310 | Enterprise Routing Capstone | Expert | Integrated design tradeoffs; fault localization; evidence-based remediation |

### Lab 301: The Feasibility Condition

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 105 and 204: RD/FD, metrics and path steering.
- **New concepts:** Strict feasibility condition; feasible successor selection.
- **Learning objectives / student work:** Create and identify a feasible successor without displacing the preferred successor; predict fast failover.
- **Starting configuration:** Four-router diamond with known link costs; initial successor established and alternate available.
- **Expected observable outcome:** The alternate meets RD < FD and becomes an immediate successor after primary failure without destination-specific QUERY emission.

### Lab 302: Variance and Unequal-Cost Routing

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 106 and 301: ECMP and feasibility.
- **New concepts:** Variance admission; unequal-cost installed paths; feasibility versus cost threshold.
- **Learning objectives / student work:** Admit eligible unequal-cost routes and prove why an attractive but non-feasible path is excluded.
- **Starting configuration:** Four-router multipath topology with deterministic costs and at least one deliberately non-feasible path; variance unset.
- **Expected observable outcome:** Installed path set obeys both feasibility and variance requirements; actual forwarding behavior is documented without assuming Cisco-only traffic-share features.

### Lab 303: DUAL Diffusing Computation

- **Difficulty:** Expert
- **Prerequisite knowledge:** 107 and 301: convergence and feasibility.
- **New concepts:** ACTIVE/PASSIVE transitions; QUERY and REPLY dependencies.
- **Learning objectives / student work:** Induce loss of all feasible alternatives; trace the DUAL state progression and find the new successor.
- **Starting configuration:** Diamond/kite network with a known destination and deliberately chosen metric relationships; packet/state observability enabled.
- **Expected observable outcome:** Student correlates triggered QUERY/REPLY exchange, ACTIVE transition and stable PASSIVE result with the route change.

### Lab 304: SIA and Fault Isolation

- **Difficulty:** Expert
- **Prerequisite knowledge:** 303: QUERY/REPLY and ACTIVE states.
- **New concepts:** Stuck-in-active detection; delayed/missing REPLY; failure scoping.
- **Learning objectives / student work:** Diagnose a delayed or blocked reply, distinguish genuine SIA from ordinary convergence, and restore healthy operation.
- **Starting configuration:** Multi-hop topology with working routes and a fault injection capability for delayed/blocked EIGRP exchange.
- **Expected observable outcome:** Identify the affected dependency and protocol evidence, explain neighbor/reset behavior, and prove stable recovery.

### Lab 305: Dual Failures and Query Scope

- **Difficulty:** Expert
- **Prerequisite knowledge:** 301–304: feasible successors and diffusing computation.
- **New concepts:** Sequential/simultaneous failure interactions; bounded query domain.
- **Learning objectives / student work:** Predict and troubleshoot overlapping failures, isolate unexpected queries and prevent persistent stale forwarding.
- **Starting configuration:** Six-to-eight-router redundant network with established policies and specific failure scripts; baseline converged.
- **Expected observable outcome:** State, next hops, withdraw/recovery and relevant packet observations satisfy expectations after each failure stage.

### Lab 306: Named Mode for IPv4

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 102, 203 and 208: EIGRP configuration and policy.
- **New concepts:** Named EIGRP hierarchy; address-family, AF-interface and topology configuration.
- **Learning objectives / student work:** Translate familiar IPv4 routing behavior into named-mode configuration and prove the same operational results.
- **Starting configuration:** Four-router IPv4 network fully addressed; routing baseline provided in reference form; named-mode EIGRP config assigned to the student.
- **Expected observable outcome:** Named-mode adjacencies and routes converge; intended per-interface and topology settings are active rather than merely stored.

### Lab 307: Named Mode for IPv6

- **Difficulty:** Advanced
- **Prerequisite knowledge:** 306: named-mode hierarchy; IPv6 addressing fundamentals.
- **New concepts:** IPv6 AF operation; link-local peers and next hops; IPv6 prefix advertisements.
- **Learning objectives / student work:** Build IPv6 EIGRP on a prepared dual-stack topology and validate neighbor and remote-prefix behavior.
- **Starting configuration:** Three-router dual-stack lab with IPv6 addresses and router IDs allocated; IPv4 baseline working, IPv6 EIGRP not yet configured.
- **Expected observable outcome:** IPv6 EIGRP neighbors establish, remote global prefixes propagate, and IPv6 next hops and routes are correct.

### Lab 308: Named Authentication and Recovery

- **Difficulty:** Expert
- **Prerequisite knowledge:** 212 and 306–307: keychains and named AF.
- **New concepts:** Authentication configuration in named mode; failure and recovery across AFs.
- **Learning objectives / student work:** Apply supported named-mode authentication, diagnose mismatches and perform controlled recovery.
- **Starting configuration:** Two- or three-router named-mode lab with IPv4/IPv6 paths and working key service; required security settings omitted.
- **Expected observable outcome:** Authenticated peer behavior matches configured policy and both rejection and recovery cases are demonstrable.

### Lab 309: IPv6 Next Hop Across AS Boundaries

- **Difficulty:** Expert
- **Prerequisite knowledge:** 208–209 and 307: AS redistribution, next hops, IPv6.
- **New concepts:** IPv6 redistribution attribution; next-hop reachability on multiaccess links.
- **Learning objectives / student work:** Examine and correct cross-AS IPv6 next-hop behavior under redistribution and asymmetric topology conditions.
- **Starting configuration:** Four routers with two IPv6 EIGRP AS domains, shared segment and seed networks; redistribution operating but next-hop policy to be validated.
- **Expected observable outcome:** External routes and resolved next hops obey the required forwarding behavior; invalid third-party next hops are not installed.

### Lab 310: Enterprise Routing Capstone

- **Difficulty:** Expert
- **Prerequisite knowledge:** 301–309: advanced DUAL, redistribution, policy, IPv6, authentication.
- **New concepts:** Integrated design tradeoffs; fault localization; evidence-based remediation.
- **Learning objectives / student work:** Diagnose a preconfigured multi-site topology containing multiple independent routing faults; make only justified corrections.
- **Starting configuration:** Eight-plus-router mixed-role lab, IPv4 and IPv6 where supported, with policy, summarization, redistribution and planted faults.
- **Expected observable outcome:** All documented reachability, control-plane, convergence and security objectives pass, with an evidence-based explanation of root causes and fixes.

## OEIGRP 400: Developing with OpenEIGRP

**Level:** Developer / maintainer laboratory
**Course outcome:** Teach contributors how to build, inspect, test and safely modify portable EIGRP and its host integrations.
**Entry:** Strong C and Python fundamentals, routing-protocol experience, Git, Linux development tooling, and knowledge equivalent to the 100–300 networking curriculum.
**Labs:** 10

| Lab | Title | Difficulty | Primary new concept |
|---|---|---|---|
| 401 | Architecture and Ownership Boundaries | Developer introduction | Portable eigrp/code versus frr/unix/openwrt/bird host adapters; northbound/southbound contracts |
| 402 | Building and Running the Core | Developer introduction | Build targets, portable unit binaries, platform build requirements and diagnostics |
| 403 | Public APIs and Configuration Lifecycle | Developer intermediate | EIGRP-owned public APIs; set/reset and add/remove; instance/AF/interface lifecycle |
| 404 | Writing a UUT YAML Scenario | Developer intermediate | eigrp-uut-topology/v1 and eigrp-uut-test/v1; virtual wire; deterministic expectations |
| 405 | Packet Traces, Events and DUAL Diagnostics | Developer intermediate | Packet journal, packet marks, event ring, state snapshots, RTP retransmission evidence |
| 406 | Regression-Quality Bug Fixing | Developer advanced | Root-cause tracing; DNDB/DRDB lifecycle; ownership/retirement; diff review |
| 407 | FRR Configuration and RIB Integration | Developer advanced | FRR daemon lifecycle; CLI/YANG dispatch; host policy and RIB callbacks |
| 408 | Unix and OpenWrt Integration Patterns | Developer advanced | Portable southbound contracts, sockets/timers/event loops and service-specific responsibilities |
| 409 | Docker, netlab and Release Validation | Developer advanced | Container daemon configuration, netlab configuration templates, CI gates and reproducibility |
| 410 | Scale, Churn and Convergence Engineering | Developer expert | High-neighbor and high-prefix stress; RTP queues; topology ownership; time-to-quiescence |

### Lab 401: Architecture and Ownership Boundaries

- **Difficulty:** Developer introduction
- **Prerequisite knowledge:** C, modules, EIGRP protocol principles.
- **New concepts:** Portable eigrp/code versus frr/unix/openwrt/bird host adapters; northbound/southbound contracts.
- **Learning objectives / student work:** Trace a configured route from CLI or host API through the portable engine and out to the host RIB.
- **Starting configuration:** Read-only repository checkout, architecture/specification files and a working small example; no code modifications required.
- **Expected observable outcome:** Produce a verified call-path map with owner of DUAL, topology, packetization, interface/RIB services and lifecycle.

### Lab 402: Building and Running the Core

- **Difficulty:** Developer introduction
- **Prerequisite knowledge:** 401: ownership boundaries; C toolchains and make.
- **New concepts:** Build targets, portable unit binaries, platform build requirements and diagnostics.
- **Learning objectives / student work:** Build the supported portable target, run existing unit tests and understand how failures are reported.
- **Starting configuration:** Pinned checkout on an explicitly chosen branch, compiler/Python dependencies and no local changes.
- **Expected observable outcome:** Build completes on the chosen host; targeted tests execute and their exact commands and results are documented.

### Lab 403: Public APIs and Configuration Lifecycle

- **Difficulty:** Developer intermediate
- **Prerequisite knowledge:** 401–402: architecture and working build.
- **New concepts:** EIGRP-owned public APIs; set/reset and add/remove; instance/AF/interface lifecycle.
- **Learning objectives / student work:** Trace and exercise one supported configuration API through implementation, error path and teardown.
- **Starting configuration:** Instrumented portable sample program or established test fixture; initial instance and test interfaces defined.
- **Expected observable outcome:** API call reaches its owning implementation, modifies actual protocol state, and handles invalid inputs and cleanup correctly.

### Lab 404: Writing a UUT YAML Scenario

- **Difficulty:** Developer intermediate
- **Prerequisite knowledge:** 402–403: local tests and instance lifecycle.
- **New concepts:** eigrp-uut-topology/v1 and eigrp-uut-test/v1; virtual wire; deterministic expectations.
- **Learning objectives / student work:** Author a small executable UUT topology and scenario for withdrawal, recovery and negative packet assertions.
- **Starting configuration:** Working eigrp-uut-node harness and sample topology; test task specified but scenario not implemented.
- **Expected observable outcome:** New test executes actual portable EIGRP behavior, distinguishes failure from pass, and records repeatable diagnostics.

### Lab 405: Packet Traces, Events and DUAL Diagnostics

- **Difficulty:** Developer intermediate
- **Prerequisite knowledge:** 404: UUT scenarios; RTP/DUAL fundamentals.
- **New concepts:** Packet journal, packet marks, event ring, state snapshots, RTP retransmission evidence.
- **Learning objectives / student work:** Instrument a convergence problem and correlate emitted packets with DUAL mutation-site events.
- **Starting configuration:** Known multi-router fault scenario and protocol logging available; diagnostics not yet collected.
- **Expected observable outcome:** Produce a timeline of link/fault, QUERY/REPLY, state transition and resulting RIB changes with no inferred events.

### Lab 406: Regression-Quality Bug Fixing

- **Difficulty:** Developer advanced
- **Prerequisite knowledge:** 403–405: API ownership, test authoring and diagnostics.
- **New concepts:** Root-cause tracing; DNDB/DRDB lifecycle; ownership/retirement; diff review.
- **Learning objectives / student work:** Reproduce a bounded seeded defect, fix only its root cause, and supply executable plus YAML scenario coverage.
- **Starting configuration:** Dedicated named exercise branch/check-out and seeded defect fixture; expected behavior documented with RFC/observed evidence.
- **Expected observable outcome:** Demonstrate fail-before/pass-after where practical, targeted and existing regression results, cleanup checks and a complete-file overlay; no remote writes required.

### Lab 407: FRR Configuration and RIB Integration

- **Difficulty:** Developer advanced
- **Prerequisite knowledge:** 401–406: portable contracts, tests and regression discipline.
- **New concepts:** FRR daemon lifecycle; CLI/YANG dispatch; host policy and RIB callbacks.
- **Learning objectives / student work:** Trace a real FRR EIGRP configuration from CLI to owning portable target and verify route installation.
- **Starting configuration:** FRR integration build image or fixture, known-good configuration and a contained enhancement/bug exercise.
- **Expected observable outcome:** Show real config effect, error reporting and correct host RIB behavior; execute integration and portable regressions.

### Lab 408: Unix and OpenWrt Integration Patterns

- **Difficulty:** Developer advanced
- **Prerequisite knowledge:** 407: host-adapter concepts and interface/RIB lifecycle.
- **New concepts:** Portable southbound contracts, sockets/timers/event loops and service-specific responsibilities.
- **Learning objectives / student work:** Compare Unix and OpenWrt host interfaces and implement or verify one small adapter-facing lifecycle path.
- **Starting configuration:** Working sample integration harness, portable engine and a selected interface/RIB lifecycle exercise.
- **Expected observable outcome:** Selected adapter path creates, updates and cleans up actual host state without leaking platform dependencies into eigrp/code.

### Lab 409: Docker, netlab and Release Validation

- **Difficulty:** Developer advanced
- **Prerequisite knowledge:** 404–408: tested builds and host integrations.
- **New concepts:** Container daemon configuration, netlab configuration templates, CI gates and reproducibility.
- **Learning objectives / student work:** Package and boot the FRR image under its defaults and an externally managed daemon file; run a small netlab validation.
- **Starting configuration:** Buildable FRR Docker image, containerlab/netlab host and an example topology; external config override fixture supplied.
- **Expected observable outcome:** Container startup and daemon supervision have clear diagnostics; adjacency/route assertions pass when enabled; logs and versioned artifacts are reproducible.

### Lab 410: Scale, Churn and Convergence Engineering

- **Difficulty:** Developer expert
- **Prerequisite knowledge:** 405–409: diagnostics, regression workflow and deployed labs.
- **New concepts:** High-neighbor and high-prefix stress; RTP queues; topology ownership; time-to-quiescence.
- **Learning objectives / student work:** Run a bounded scale/stress scenario, identify bottlenecks, and differentiate failure, convergence and cleanup correctness.
- **Starting configuration:** Existing portable scale harness and selected netlab topology, known workload profile and available resource limits.
- **Expected observable outcome:** Report neighbor/prefix counts, convergence metrics, queue behavior, memory ownership and evidence-backed regression findings.

## Instructional progression and assessment

- **100 level:** Guided, short tasks. Learners should be able to identify neighbors, chosen routes and basic fault conditions by reading operational state.
- **200 level:** Semi-guided policy design. Learners must explain why a prefix is advertised, suppressed or preferred and demonstrate no unintended side effects.
- **300 level:** Challenge-driven diagnostics. Learners must predict DUAL and forwarding behavior before changes, collect evidence during failures and justify configuration choices.
- **400 level:** Engineering practice. Learners must provide reproducible builds, lifecycle tracing, executable tests, failure reports and regression evidence, not merely configuration screenshots.

**No compulsory repeated setup:** Every lab's baseline restores previous knowledge by supplying addressing, interfaces, routing policy or source code fixtures except the precise topic being taught. Each README may *review* an earlier concept, but an advanced assessment cannot spend most of its work on introductory configuration.

## Publishing and platform qualification

Use a central catalog with per-platform statuses: `planned`, `in progress`, `validated`, or `not qualified`. Label each published reference result with the OpenEIGRP branch/commit, image tag, netlab version, host platform, and exact automated validation result. Keep platform-specific configuration examples behind their own adapters, including CLI differences for FRR, Unix and OpenWrt. Cisco interoperability may be offered as an optional vendor-dependent variant with appropriate image/licensing requirements.

The learning objectives are proposed from the user-supplied *EIGRP Routing Protocol WorkBook*, linked CCNP EIGRP examples discussed in planning, and OpenEIGRP's existing UUT/architecture/testing model. The curriculum is an independently written sequence and deliberately does not reproduce the workbook's solutions or lab numbering.
