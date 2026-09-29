# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Direct behavior coverage for redistribution seed-metric precedence.

from pathlib import Path
import os
import subprocess
import sys
import textwrap


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def test_redistribution_seed_metric_precedence_executes(tmp_path):
    source = tmp_path / "redistribute_seed_metric.c"
    binary = tmp_path / "redistribute_seed_metric"
    source.write_text(
        textwrap.dedent(
            r'''
            #include <assert.h>
            #include <stdint.h>
            #include <string.h>

            /* Pull the real private configuration layouts and selector into
             * this focused translation unit.  Link-time GC discards unrelated
             * protocol functions from these implementation files.
             */
            #include "eigrpd/code/eigrp_metric.c"
            #include "eigrpd/code/eigrp_redistribute.c"

            static eigrp_af_instance_t *test_runtime_af;

            void eigrp_log(eigrp_log_level_t level, const char *format, ...)
            {
                (void)level;
                (void)format;
            }

            eigrp_result_t eigrp_topology_redistributed_route_update(
                eigrp_instance_t *runtime,
                const eigrp_rib_source_route_t *route,
                const eigrp_metrics_t *metric)
            {
                (void)runtime;
                (void)route;
                (void)metric;
                return EIGRP_RESULT_SUCCESS;
            }

            eigrp_result_t eigrp_topology_redistributed_route_remove(
                eigrp_instance_t *runtime,
                const eigrp_rib_source_route_t *route)
            {
                (void)runtime;
                (void)route;
                return EIGRP_RESULT_SUCCESS;
            }

            eigrp_prefix_descriptor_t *eigrp_topology_table_lookup(
                eigrp_table_t *table, const eigrp_prefix_t *prefix)
            {
                (void)table;
                (void)prefix;
                return NULL;
            }

            eigrp_route_descriptor_t *eigrp_prefix_descriptor_lookup(
                eigrp_prefix_descriptor_t *prefix, eigrp_nbr_t *neighbor)
            {
                (void)prefix;
                (void)neighbor;
                return NULL;
            }

            bool eigrp_topology_prefix_admit(
                eigrp_instance_t *runtime, const eigrp_prefix_t *prefix)
            {
                (void)runtime;
                (void)prefix;
                return true;
            }


            eigrp_result_t eigrp_sys_redistribute_route_map_evaluate(
                eigrp_instance_t *eigrp, const char *name,
                const eigrp_rib_source_route_t *route, eigrp_filter_decision_t *decision)
            {
                (void)eigrp;
                (void)name;
                (void)route;
                if (decision)
                    *decision = EIGRP_FILTER_DECISION_PERMIT;
                return EIGRP_RESULT_SUCCESS;
            }

            bool eigrp_instance_data_path_ready(const eigrp_instance_t *runtime)
            {
                return runtime != NULL;
            }

            eigrp_afi_t
            eigrp_instance_afi(const eigrp_instance_t *runtime)
            {
                (void)runtime;
                return EIGRP_AFI_IPV4;
            }

            eigrp_af_instance_t *
            eigrp_instance_runtime_config(eigrp_instance_t *runtime)
            {
                (void)runtime;
                return test_runtime_af;
            }

            uint16_t eigrp_instance_asn(const eigrp_instance_t *runtime)
            {
                (void)runtime;
                return 4453;
            }

            bool eigrp_prefix_valid(const eigrp_prefix_t *prefix)
            {
                return prefix != NULL
                       && prefix->address.afi == EIGRP_AFI_IPV4
                       && prefix->prefix_length <= 32;
            }

            static void assert_seed_values(const eigrp_metrics_t *metric,
                                           const eigrp_metric_values_t *values)
            {
                assert(metric->bandwidth ==
                       eigrp_bandwidth_to_scaled(values->bandwidth));
                assert(metric->delay == values->delay);
                assert(metric->reliability == values->reliability);
                assert(metric->load == values->load);
                assert(metric->mtu[0] == (values->mtu & 0xff));
                assert(metric->mtu[1] == ((values->mtu >> 8) & 0xff));
                assert(metric->mtu[2] == 0);
                assert(metric->hop_count == 0);
                assert(metric->tag == 0);
                assert(metric->flags == 0);
            }

            int main(void)
            {
                eigrp_metric_config_t metric_config = {0};
                eigrp_af_instance_t af = {
                    .afi = EIGRP_AFI_IPV4,
                    .asn = 4453,
                    .metric_config = &metric_config,
                };
                eigrp_redist_config_t config = {0};
                eigrp_rib_source_route_t route = {0};
                eigrp_metrics_t selected;
                eigrp_metric_values_t explicit_metric = {
                    .bandwidth = 10000,
                    .delay = 101,
                    .reliability = 250,
                    .load = 11,
                    .mtu = 1500,
                };
                eigrp_metric_values_t default_metric = {
                    .bandwidth = 20000,
                    .delay = 202,
                    .reliability = 240,
                    .load = 22,
                    .mtu = 1400,
                };
                eigrp_metrics_t source_vector = {
                    .delay = 0x0000112233445566ULL,
                    .bandwidth = 0x0000123456789abcULL,
                    .mtu = {0x34, 0x12, 0x01},
                    .hop_count = 17,
                    .reliability = 201,
                    .load = 33,
                    .tag = 77,
                    .flags = 0xa5,
                };
                eigrp_redist_metric_origin_t origin;

                metric_config.default_metric_configured = true;
                metric_config.default_metric = default_metric;
                config.source.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP;
                config.source.route_instance = 100;
                route.source = config.source;
                route.eigrp_vector_present = true;
                route.eigrp_vector = source_vector;

                /* 1. Explicit redistribute metric overrides both an EIGRP
                 * source vector and default-metric.
                 */
                config.metric_configured = true;
                config.metric = explicit_metric;
                origin = eigrp_redist_metric_select(
                    &af, &config, &route, &selected);
                assert(origin == EIGRP_REDISTRIBUTE_METRIC_EXPLICIT);
                assert_seed_values(&selected, &explicit_metric);

                /* 2. Without an explicit override, preserve the complete
                 * native EIGRP vector.  The 64-bit components and 24-bit MTU
                 * values intentionally cannot survive a five-value seed tuple.
                 */
                config.metric_configured = false;
                memset(&selected, 0, sizeof(selected));
                origin = eigrp_redist_metric_select(
                    &af, &config, &route, &selected);
                assert(origin == EIGRP_REDISTRIBUTE_METRIC_SOURCE_EIGRP);
                assert(memcmp(&selected, &source_vector,
                              sizeof(source_vector)) == 0);

                /* 3. An unusable EIGRP source vector falls through to the
                 * configured default-metric.
                 */
                route.eigrp_vector.bandwidth = 0;
                memset(&selected, 0, sizeof(selected));
                origin = eigrp_redist_metric_select(
                    &af, &config, &route, &selected);
                assert(origin == EIGRP_REDISTRIBUTE_METRIC_DEFAULT);
                assert_seed_values(&selected, &default_metric);

                /* A foreign protocol also uses the default-metric path. */
                route.source.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_OSPF;
                route.source.route_instance = 10;
                route.eigrp_vector = source_vector;
                memset(&selected, 0, sizeof(selected));
                origin = eigrp_redist_metric_select(
                    &af, &config, &route, &selected);
                assert(origin == EIGRP_REDISTRIBUTE_METRIC_DEFAULT);
                assert_seed_values(&selected, &default_metric);

                /* 4. With no usable vector and no configured seed, the route
                 * is not importable.  A nonzero scalar RIB metric must not be
                 * expanded into invented EIGRP vector components.
                 */
                metric_config.default_metric_configured = false;
                route.metric = UINT64_C(0x123456789abcdef0);
                memset(&selected, 0xff, sizeof(selected));
                origin = eigrp_redist_metric_select(
                    &af, &config, &route, &selected);
                assert(origin == EIGRP_REDISTRIBUTE_METRIC_NONE);
                {
                    eigrp_metrics_t zero = {0};
                    assert(memcmp(&selected, &zero, sizeof(zero)) == 0);
                }

                /* The public ingress treats that absence as a normal ignored
                 * candidate, not invalid input or malformed topology state.
                 */
                route.prefix.address.afi = EIGRP_AFI_IPV4;
                route.prefix.prefix_length = 24;
                config.source = route.source;
                config.next = NULL;
                af.redistributions = &config;
                test_runtime_af = &af;
                {
                    eigrp_instance_t runtime = {0};
                    assert(eigrp_rib_source_route_add(&runtime, &route)
                           == EIGRP_RESULT_SUCCESS);
                }

                return 0;
            }
            '''
        )
    )

    compiler = os.environ.get("CC", "cc")
    result = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffunction-sections",
            "-fdata-sections",
            f"-I{ROOT}",
            f"-I{ROOT / 'test' / 'build' / 'include'}",
            str(source),
            *( ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"] ),
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr

    result = subprocess.run(
        [str(binary)],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr
