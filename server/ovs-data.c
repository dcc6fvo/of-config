
/* Copyright (c) 2015 Open Networking Foundation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <assert.h>
#include <sys/socket.h>
#include <linux/ethtool.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

/* libovs */
#include <dynamic-string.h>
#include <ovsdb-idl-provider.h>
#include <vswitch-idl.h>
#include <dpif.h>
#include <vconn.h>
#include <socket-util.h>
#include <ofp-util.h>
#include <ofp-msgs.h>
#include <poll-loop.h>

#include <libnetconf/netconf.h>

#include "data.h"

typedef struct {
    struct ovsdb_idl *idl;
    unsigned int seqno;
    struct vconn *vconn;
    ofc_resmap_t *resource_map;
} ovsdb_t;
ovsdb_t *ovsdb_handler = NULL;

int ioctlfd = -1;

struct u32_str_map{
    uint32_t value;
    const char *str;
};

static const char *
print_uuid_ro(const struct uuid *uuid)
{
    static char str[38];

    snprintf(str, 37, UUID_FMT, UUID_ARGS(uuid));
    str[37] = 0;
    return str;
}
#if 0
/* unused */
static char *
print_uuid(const struct uuid *uuid)
{
    return strdup(print_uuid_ro(uuid));
}
#endif

/*
 * If key is in the string map s, append the it's value into string,
 * otherwise don't append anything.
 *
 * s    string map with data
 * key  value of key in string map to find
 * elem name of element to append into XML
 * string   dynamic string containing XML
 */
static void
find_and_append_smap_val(const struct smap *s, const char *key,
                         const char *elem, struct ds *string)
{

    const char *value = smap_get(s, key);
    if (value != NULL) {
        ds_put_format(string, "<%s>%s</%s>", elem, value, elem);
    }
}

/*
 * Look up resource-id by uuid in the rm map, generate new if missing.
 * \param[in,out] rm    pointer to the rm map
 * \param[in] uuid      find existing record by uuid
 * \return Non-empty string with found or generated resource-id.
 * Empty string "" when no resource-id found and insertion of the
 * new one failed.  Resource-id is generated by converting UUID into string.
 */
static const char *
find_resid_generate(ofc_resmap_t *rm, const struct uuid *uuid)
{
    bool result;
    const char *resource_id = NULL;
    ofc_tuple_t *found = ofc_resmap_find_u(rm, uuid);
    if (found == NULL) {
        /* generate new resource-id (UUID string) */
        resource_id = print_uuid_ro(uuid);
        /* insert new record */
        result = ofc_resmap_insert(rm, resource_id, uuid);
        if (result == true) {
            return resource_id;
        } else {
            return "";
        }
    } else {
        return found->resource_id;
    }
}


static char *
get_flow_tables_state(void)
{
    const struct ovsrec_flow_table *row, *next;
    struct ds string;
    const char *resource_id;

    ds_init(&string);
    OVSREC_FLOW_TABLE_FOR_EACH_SAFE(row, next, ovsdb_handler->idl) {
        resource_id = find_resid_generate(ovsdb_handler->resource_map,
                                          &row->header_.uuid);
        ds_put_format(&string, "<flow-table><resource-id>%s</resource-id>"
                      "<max-entries>%"PRId64"</max-entries></flow-table>",
                      resource_id,
                      (row->n_flow_limit > 0 ? row->flow_limit[0] : 0));
    }
    return ds_steal_cstr(&string);
}

static char *
get_flow_tables_config(void)
{
    /* TODO flow-table "<flow-table>"
     * "<table-id>%s</table-id>"
     "</flow-table>" */

    const struct ovsrec_flow_table *row, *next;
    const char *resource_id;
    struct ds string;

    ds_init(&string);
    OVSREC_FLOW_TABLE_FOR_EACH_SAFE(row, next, ovsdb_handler->idl) {
        resource_id = find_resid_generate(ovsdb_handler->resource_map,
                                          &row->header_.uuid);
        ds_put_format(&string, "<flow-table><resource-id>%s</resource-id>",
                      resource_id);
        ds_put_format(&string, "<name>%s</name", row->name);
        ds_put_format(&string, "</flow-table>");
    }
    return ds_steal_cstr(&string);
}

static char *
get_queues_config(void)
{
    /* TODO
       "<queue><id>%s</id>"
       "<port>%s</port>"
       "<properties>"
       "<experimenter-id>%s</experimenter-id>"
       "<experimenter-data>%s</experimenter-data>"
       "</properties></queue>"
       */
    const struct ovsrec_queue *row, *next;
    struct ds string;
    const char *resource_id;

    ds_init(&string);
    OVSREC_QUEUE_FOR_EACH_SAFE(row, next, ovsdb_handler->idl) {
        resource_id = find_resid_generate(ovsdb_handler->resource_map,
                                          &row->header_.uuid);

        ds_put_format(&string,
                      "<queue><resource-id>%s</resource-id><properties>",
                      resource_id);
        find_and_append_smap_val(&row->other_config, "min-rate", "min-rate",
                                 &string);
        find_and_append_smap_val(&row->other_config, "max-rate", "max-rate",
                                 &string);
        ds_put_format(&string, "</properties></queue>");
    }
    return ds_steal_cstr(&string);
}

static void
dump_port_features(struct ds *s, uint32_t mask)
{
    int i;
    static const struct u32_str_map rates[] = {
        { ADVERTISED_10baseT_Half,       "10Mb-HD" },
        { ADVERTISED_10baseT_Full,       "10Mb-FD" },
        { ADVERTISED_100baseT_Half,      "100Mb-HD" },
        { ADVERTISED_100baseT_Full,      "100Mb-FD" },
        { ADVERTISED_1000baseT_Half,     "1Gb-HD" },
        { ADVERTISED_1000baseT_Full,     "1Gb-FD" },
        { ADVERTISED_1000baseKX_Full,    "1Gb-FD" },
//      { ADVERTISED_2500baseX_Full,     "2500baseX/Full" },
        { ADVERTISED_10000baseT_Full,    "10Gb" },
        { ADVERTISED_10000baseKX4_Full,  "10Gb" },
        { ADVERTISED_10000baseKR_Full,   "10Gb" },
//      { ADVERTISED_20000baseMLD2_Full, "20000baseMLD2/Full" },
//      { ADVERTISED_20000baseKR2_Full,  "20000baseKR2/Full" },
        { ADVERTISED_40000baseKR4_Full,  "40Gb" },
        { ADVERTISED_40000baseCR4_Full,  "40Gb" },
        { ADVERTISED_40000baseSR4_Full,  "40Gb" },
        { ADVERTISED_40000baseLR4_Full,  "40Gb" },
    };
    static const struct u32_str_map medium[] = {
        { ADVERTISED_TP,    "copper" },
        { ADVERTISED_FIBRE, "fiber" },
    };

    assert(s);

    /* dump rate elements */
    for (i = 0; i < (sizeof rates) / (sizeof rates[0]); i++) {
        if (rates[i].value & mask) {
            ds_put_format(s, "<rate>%s</rate>", rates[i].str);
        }
    }

    /* dump auto-negotiate element */
    ds_put_format(s, "<auto-negotiate>%s</auto-negotiate>",
                  ADVERTISED_Autoneg & mask ? "true" : "false");

    /* dump medium elements */
    for (i = 0; i < (sizeof medium) / (sizeof medium[0]); i++) {
        if (medium[i].value & mask) {
            ds_put_format(s, "<medium>%s</medium>", medium[i].str);
        }
    }

    /* dump pause element */
    if (ADVERTISED_Asym_Pause & mask) {
        ds_put_format(s, "<pause>asymetric</pause>");
    } else if (ADVERTISED_Pause & mask) {
        ds_put_format(s, "<pause>symetric</pause>");
    } else {
        ds_put_format(s, "<pause>unsuported</pause>");
    }
}

static char *
get_ports_config(void)
{
    const struct ovsrec_interface *row, *next;
    struct ds string;

    ds_init(&string);
    OVSREC_INTERFACE_FOR_EACH_SAFE(row, next, ovsdb_handler->idl) {

        ds_put_format(&string, "<port>");
        ds_put_format(&string, "<name>%s</name>", row->name);
        ds_put_format(&string, "<requested-number>%" PRIu64 "</requested-number>",
                      (row->n_ofport_request > 0 ? row->ofport_request[0] : 0));
        ds_put_format(&string, "<configuration>");

        /* get interface status via ioctl() */
        struct ifreq ethreq;
        memset(&ethreq, 0, sizeof ethreq);
        strncpy(ethreq.ifr_name, row->name, sizeof ethreq.ifr_name);
        ioctl(ioctlfd, SIOCGIFFLAGS, &ethreq);
        ds_put_format(&string, "<admin-state>%s</admin-state>",
                      ethreq.ifr_flags & IFF_UP ? "up" : "down");
        /* TODO openflow:
           ds_put_format(&string, "<no-receive>%s</no-receive>", "XXX");
           ds_put_format(&string, "<no-forward>%s</no-forward>", "XXX");
           ds_put_format(&string, "<no-packet-in>%s</no-packet-in>", "XXX");
           */
        ds_put_format(&string, "</configuration>");

        /* get interface features via ioctl() */
        struct ethtool_cmd ecmd;
        memset(&ecmd, 0, sizeof ecmd);
        ecmd.cmd = ETHTOOL_GSET;
        ethreq.ifr_data = &ecmd;
        ioctl(ioctlfd, SIOCETHTOOL, &ethreq);
        ds_put_format(&string, "<features><advertised>");
        dump_port_features(&string, ecmd.advertising);
        ds_put_format(&string, "</advertised></features>");

        if (!strcmp(row->type, "gre")) {
            ds_put_format(&string, "<ipgre-tunnel>");
            find_and_append_smap_val(&row->options, "local_ip",
                                     "local-endpoint-ipv4-adress", &string);
            find_and_append_smap_val(&row->options, "remote_ip",
                                     "remote-endpoint-ipv4-adress", &string);
            find_and_append_smap_val(&row->options, "csum",
                                     "checksum-present", &string);
            find_and_append_smap_val(&row->options, "key", "key", &string);
            ds_put_format(&string, "</ipgre-tunnel>");
        } else if (!strcmp(row->type, "vxlan")) {
            ds_put_format(&string, "<vxlan-tunnel>");
            find_and_append_smap_val(&row->options, "local_ip",
                                     "local-endpoint-ipv4-adress", &string);
            find_and_append_smap_val(&row->options, "remote_ip",
                                     "remote-endpoint-ipv4-adress", &string);

            find_and_append_smap_val(&row->options, "key", "vni", &string);
            ds_put_format(&string, "</vxlan-tunnel>");
        } else if ((!strcmp(row->type, "gre64"))
                    || (!strcmp(row->type, "geneve"))
                    || (!strcmp(row->type, "lisp"))) {
            ds_put_format(&string, "<tunnel>");
            find_and_append_smap_val(&row->options, "local_ip",
                                     "local-endpoint-ipv4-adress", &string);
            find_and_append_smap_val(&row->options, "remote_ip",
                                     "remote-endpoint-ipv4-adress", &string);
            ds_put_format(&string, "</tunnel>");
        }
        ds_put_format(&string, "</port>");
    }
    return ds_steal_cstr(&string);
}

static char *
get_ports_state(void)
{
    const struct ovsrec_interface *row, *next;
    struct ds string;

    ds_init(&string);
    OVSREC_INTERFACE_FOR_EACH_SAFE(row, next, ovsdb_handler->idl) {

        /* get interface status via ioctl() */
        struct ifreq ethreq;
        struct ethtool_cmd ecmd;
        memset(&ethreq, 0, sizeof ethreq);
        memset(&ecmd, 0, sizeof ecmd);
        strncpy(ethreq.ifr_name, row->name, sizeof ethreq.ifr_name);
        ecmd.cmd = ETHTOOL_GSET;
        ethreq.ifr_data = &ecmd;
        ioctl(ioctlfd, SIOCETHTOOL, &ethreq);

        ds_put_format(&string, "<port>");
        ds_put_format(&string, "<name>%s</name>", row->name);
        ds_put_format(&string, "<number>%" PRIu64 "</number>",
                      (row->n_ofport > 0 ? row->ofport[0] : 0));
        /* TODO openflow:
           <current-rate>%s</current-rate>
           <max-rate>%s</max-rate>
           originally via ioctl(), but of provides values as curr_speed and
           max_speed in struct ofp_port when no standard speed is set, it also
           can accumulate speeds in case of port aggregation, which I'm not
           sure is possible to get via ioctl()
         */
        ds_put_format(&string, "<state>");
        ds_put_format(&string, "<oper-state>%s</oper-state>",
                      (row->link_state != NULL ? row->link_state : "down"));

        find_and_append_smap_val(&row->other_config, "stp_state", "blocked",
                                 &string);
        /* TODO openflow:
            <live>%s</live> */
        ds_put_format(&string, "</state>");

        ds_put_format(&string, "<features><current>");
        /* rate
         * - get speed and convert it with duplex value to OFPortRateType
         */
        switch ((ecmd.speed_hi << 16) | ecmd.speed) {
        case 10:
            ds_put_format(&string, "<rate>10Mb");
            break;
        case 100:
            ds_put_format(&string, "<rate>100Mb");
            break;
        case 1000:
            ds_put_format(&string, "<rate>1Gb");
            break;
        case 10000:
            ds_put_format(&string, "<rate>10Gb");
            ecmd.duplex = DUPLEX_FULL + 1; /* do not print duplex suffix */
            break;
        case 40000:
            ds_put_format(&string, "<rate>40Gb");
            ecmd.duplex = DUPLEX_FULL + 1; /* do not print duplex suffix */
            break;
        default:
            ds_put_format(&string, "<rate>");
            ecmd.duplex = DUPLEX_FULL + 1; /* do not print duplex suffix */
        }
        switch (ecmd.duplex) {
        case DUPLEX_HALF:
            ds_put_format(&string, "-HD</rate>");
            break;
        case DUPLEX_FULL:
            ds_put_format(&string, "-FD</rate>");
            break;
        default:
            ds_put_format(&string, "</rate>");
            break;
        }

        /* auto-negotiation */
        ds_put_format(&string, "<auto-negotiate>%s</auto-negotiate>",
                      ecmd.autoneg ? "true" : "false");
        /* medium */
        switch(ecmd.port) {
        case PORT_TP:
            ds_put_format(&string, "<medium>copper</medium>");
            break;
        case PORT_FIBRE:
            ds_put_format(&string, "<medium>fiber</medium>");
            break;
        }

        /* pause is filled with the same value as in advertised */
        if (ADVERTISED_Asym_Pause & ecmd.advertising) {
            ds_put_format(&string, "<pause>asymetric</pause>");
        } else if (ADVERTISED_Pause & ecmd.advertising) {
            ds_put_format(&string, "<pause>symetric</pause>");
        } else {
            ds_put_format(&string, "<pause>unsuported</pause>");
        }

        ds_put_format(&string, "</current><supported>");
        dump_port_features(&string, ecmd.supported);
        ds_put_format(&string, "</supported><advertised-peer>");
        dump_port_features(&string, ecmd.lp_advertising);
        ds_put_format(&string, "</advertised-peer></features>");

        ds_put_format(&string, "</port>");
    }
    return ds_steal_cstr(&string);
}

static void
get_controller_state(struct ds *string, const struct ovsrec_controller *row)
{
    ds_put_format(string, "<controller>");
    /* TODO?
       <id>%s</id>
       */
    ds_put_format(string, "<state>");
    ds_put_format(string, "<connection-state>%s</connection-state>",
                  (row->is_connected ? "up" : "down"));
    /* XXX not mapped: ds_put_format(string,
     * "<current-version>%s</current-version>", ); ds_put_format(string,
     * "<supported-versions>%s</supported-versions>", ); */
    /* XXX local-*-in-use  - TODO use netstat */
    ds_put_format(string, "<local-ip-address-in-use>%s</local-ip-address-in-use>", "XXX");
    ds_put_format(string, "<local-port-in-use>%s</local-port-in-use>", "XXX");
    ds_put_format(string, "</state>");

    ds_put_format(string, "</controller>");
}

/* parses target t: rewrites delimiters to \0 and sets output pointers */
static void
parse_target_to_addr(char *t, char **protocol, char **address, char **port)
{
    /* XXX write some test for this... */
    char *is_ipv6 = NULL;
    if (t == NULL) {
        (*protocol) = NULL;
        (*address) = NULL;
        (*port) = NULL;
    }

    /* t begins with protocol */
    (*protocol) = t;

    /* address is after delimiter ':' */
    (*address) = strchr(*protocol, ':');
    is_ipv6 = strchr(*address, '[');
    if (*address != NULL) {
        *(*address) = 0;
        (*address)++;
        if (is_ipv6 != NULL) {
            (*port) = strchr(*address, ']');
            (*port)++;
        } else {
            (*port) = strchr(*address, ':');
        }
        if (*port != NULL) {
            *(*port) = 0;
            (*port)++;
        }
    } else {
        (*port) = NULL;
    }
}

static void
get_controller_config(struct ds *string, const struct ovsrec_controller *row)
{
    char *protocol, *address, *port;
    char *target = strdup(row->target);
    const char *resource_id;

    parse_target_to_addr(target, &protocol, &address, &port);
    resource_id = find_resid_generate(ovsdb_handler->resource_map,
                                      &row->header_.uuid);

    ds_put_format(string, "<controller>");
    ds_put_format(string, "<id>%s</id>", resource_id);
    ds_put_format(string, "<ip-address>%s</ip-address>", address);
    ds_put_format(string, "<port>%s</port>", port);
    ds_put_format(string, "<protocol>%s</protocol>", protocol);

    if (!strcmp(row->connection_mode, "in-band")) {
        ds_put_format(string, "<local-ip-address>%s</local-ip-address>",
                      row->local_ip);
    }
    ds_put_format(string, "</controller>");
}

static char *
get_bridges_state(void)
{
    const struct ovsrec_bridge *row, *next;
    struct ds string;
    size_t i;

    ds_init(&string);
    OVSREC_BRIDGE_FOR_EACH_SAFE(row, next, ovsdb_handler->idl) {
        /* char *uuid = print_uuid(&row->header_.uuid); */
        /* ds_put_format(&string, "<resource-id>%s</resource-id>", uuid);
         * free(uuid); */
        ds_put_format(&string, "<switch>");
        ds_put_format(&string, "<id>%s</id>", row->name);
        ds_put_format(&string, "<capabilities><max-buffered-packets>%d"
                      "</max-buffered-packets>", 256);
        ds_put_format(&string, "<max-tables>%d</max-tables>", 255);
        ds_put_format(&string, "<max-ports>%d</max-ports>", 255);
        ds_put_format(&string, "<flow-statistics>%s</flow-statistics>",
                      "true");
        ds_put_format(&string, "<table-statistics>%s</table-statistics>",
                      "true");
        ds_put_format(&string, "<port-statistics>%s</port-statistics>",
                      "true");
        ds_put_format(&string, "<group-statistics>%s</group-statistics>",
                      "true");
        ds_put_format(&string, "<queue-statistics>%s</queue-statistics>",
                      "true");
        ds_put_format(&string,
                      "<reassemble-ip-fragments>%s</reassemble-ip-fragments>",
                      "true");
        ds_put_format(&string, "<block-looping-ports>%s</block-looping-ports>",
                      "true");

        ds_put_format(&string, "<reserved-port-types><type>all</type>"
                      "<type>controller</type><type>table</type>"
                      "<type>inport</type><type>any</type><type>normal</type>"
                      "<type>flood</type></reserved-port-types>");

        ds_put_format(&string, "<group-types><type>all</type>"
                      "<type>select</type><type>indirect</type>"
                      "<type>fast-failover</type></group-types>");

        ds_put_format(&string, "<group-capabilities>"
                      "<capability>select-weight</capability>"
                      "<capability>select-liveness</capability>"
                      "<capability>chaining-check</capability>"
                      "</group-capabilities>");

        ds_put_format(&string, "<action-types>");
        ds_put_format(&string, "<type>set-mpls-ttl</type>"
                      "<type>dec-mpls-ttl</type><type>push-vlan</type>"
                      "<type>pop-vlan</type><type>push-mpls</type>");
        ds_put_format(&string, "<type>pop-mpls</type><type>set-queue</type>"
                      "<type>group</type><type>set-nw-ttl</type>"
                      "<type>dec-nw-ttl</type><type>set-field</type>");
        ds_put_format(&string, "</action-types>");

        ds_put_format(&string, "<instruction-types>");
        ds_put_format(&string, "<type>apply-actions</type>"
                      "<type>clear-actions</type><type>write-actions</type>"
                      "<type>write-metadata</type><type>goto-table</type>");
        ds_put_format(&string, "</instruction-types>");
        ds_put_format(&string, "</capabilities>");
        if (row->n_controller > 0) {
            ds_put_format(&string, "<controllers>");
            for (i = 0; i < row->n_controller; ++i) {
                get_controller_state(&string, row->controller[i]);
            }
            ds_put_format(&string, "</controllers>");
        }
        ds_put_format(&string, "</switch>");
    }

    return ds_steal_cstr(&string);
}

void
append_resource_refs(struct ds *string, struct ovsdb_idl_row **h,
                     size_t count, const char *elem)
{
    size_t i;
    const char *resource_id;
    if (count > 0) {
        for (i = 0; i < count; ++i) {
            resource_id = find_resid_generate(ovsdb_handler->resource_map,
                                              &h[i]->uuid);
            ds_put_format(string, "<%s>%s</%s>", elem, resource_id, elem);
        }
    }
}
static char *
get_bridges_config(void)
{
    const struct ovsrec_bridge *row, *next;
    struct ovsrec_port *port;
    struct ds string;
    size_t i;

    ds_init(&string);
    OVSREC_BRIDGE_FOR_EACH_SAFE(row, next, ovsdb_handler->idl) {
        /* char *uuid = print_uuid(&row->header_.uuid); */
        /* ds_put_format(&string, "<resource-id>%s</resource-id>", uuid);
         * free(uuid); */
        ds_put_format(&string, "<switch>");
        ds_put_format(&string, "<id>%s</id>", row->name);
        /* TODO
           "<enabled>%s</enabled>"
           */
        find_and_append_smap_val(&row->other_config, "datapath-id",
                                 "datapath-id", &string);
        if (row->fail_mode != NULL) {
            ds_put_format(&string, "<lost-connection-behavior>%s"
                    "</lost-connection-behavior>", row->fail_mode);
        }
        if (row->n_controller > 0) {
            ds_put_format(&string, "<controllers>");
            for (i = 0; i < row->n_controller; ++i) {
                get_controller_config(&string, row->controller[i]);
            }
            ds_put_format(&string, "</controllers>");
        }

        ds_put_format(&string, "<resources>");
        for (i=0; i<row->n_ports; i++) {
            port = row->ports[i];
            if (port == NULL) {
                continue;
            }
            ds_put_format(&string, "<port>%s</port>", port->name);
        }
        append_resource_refs(&string,
                             (struct ovsdb_idl_row **) row->value_flow_tables,
                             row->n_flow_tables, "flow-table");

        if (row->n_ports > 0) {
            for (i = 0; i < row->n_ports; ++i) {
                if (row->ports[i]->qos != NULL) {
                    /* XXX test with some QoS'ed interface */
                    append_resource_refs(&string,
                            (struct ovsdb_idl_row **) row->ports[i]->qos
                            ->value_queues,
                            row->ports[i]->qos->n_queues, "queue");
                }
            }
        }
        /* TODO:
           "<certificate>%s</certificate>"
           */
        ds_put_format(&string, "</resources></switch>");
    }

    return ds_steal_cstr(&string);
}

char *
get_owned_certificates_config()
{
    /* TODO owned certificate "<owned-certificate>"
     * "<resource-id>%s</resource-id>" "<certificate>%s</certificate>"
     * "<private-key>" "<key-type>" "<dsa>" "<DSAKeyValue>" "<P>%s</P>"
     * "<Q>%s</Q>" "<J>%s</J>" "<G>%s</G>" "<Y>%s</Y>" "<Seed>%s</Seed>"
     * "<PgenCounter>%s</PgenCounter>" "</DSAKeyValue>" "</dsa>" "<rsa>"
     * "<RSAKeyValue>" "<Modulus>%s</Modulus>" "<Exponent>%s</Exponent>"
     * "</RSAKeyValue>" "</rsa>" "</key-type>" "</private-key>"
     * "</owned-certificate>" */
    return NULL;
}

char *
get_external_certificates_config()
{
    /* TODO external-certificate "<external-certificate>"
     * "<resource-id>%s</resource-id>" "<certificate>%s</certificate>"
     * "</external-certificate>" */
    return NULL;
}

/* synchronize local copy of OVSDB */
static void
ofconf_update(ovsdb_t *p)
{
    int retval, i;
    for (i=0; i<4; i++) {
        ovsdb_idl_run(p->idl);
        if (!ovsdb_idl_is_alive(p->idl)) {
            retval = ovsdb_idl_get_last_error(p->idl);
            nc_verb_error("OVS database connection failed (%s)",
                   ovs_retval_to_string(retval));
        }

        if (p->seqno != ovsdb_idl_get_seqno(p->idl)) {
            p->seqno = ovsdb_idl_get_seqno(p->idl);
            i--;
        }

        if (p->seqno == ovsdb_idl_get_seqno(p->idl)) {
            ovsdb_idl_wait(p->idl);
            poll_timer_wait(100); /* wait for 100ms (at most) */
            poll_block();
        }
    }
}


char *
get_config_data()
{
    const char *config_data_format = "<?xml version=\"1.0\"?><capable-switch xmlns=\"urn:onf:config:yang\">"
        "<id>%s</id><resources>" "%s"    /* port */
        "%s"                    /* queue */
        "%s"                    /* owned-certificate */
        "%s"                    /* external-certificate */
        "%s"                    /* flow-table */
        "</resources>"
        "<logical-switches>%s</logical-switches></capable-switch>";

    struct ds state_data;

    const char *id;
    char *queues;
    char *ports;
    char *flow_tables;
    char *bridges;
    char *owned_certificates;
    char *external_certificates;

    if (ovsdb_handler == NULL) {
        return NULL;
    }

    id = (const char*)ofc_get_switchid();
    if (!id) {
        /* no id -> no data */
        return strdup("");
    }

    queues = get_queues_config();
    if (queues == (NULL)) {
        queues = strdup("");
    } ports = get_ports_config();
    if (ports == (NULL)) {
        ports = strdup("");
    } flow_tables = get_flow_tables_config();
    if (flow_tables == (NULL)) {
        flow_tables = strdup("");
    } bridges = get_bridges_config();
    if (bridges == (NULL)) {
        bridges = strdup("");
    } owned_certificates = get_owned_certificates_config();
    if (owned_certificates == (NULL)) {
        owned_certificates = strdup("");
    } external_certificates = get_external_certificates_config();
    if (external_certificates == (NULL)) {
        external_certificates = strdup("");
    }

    ds_init(&state_data);

    ds_put_format(&state_data, config_data_format, id, ports, queues,
                  flow_tables, owned_certificates, external_certificates,
                  bridges);

    free(queues);
    free(ports);
    free(flow_tables);
    free(bridges);
    free(owned_certificates);
    free(external_certificates);

    return ds_steal_cstr(&state_data);
}

char *
get_state_data(xmlDocPtr running)
{
    const char *state_data_format = "<?xml version=\"1.0\"?>"
        "<capable-switch xmlns=\"urn:onf:config:yang\">"
        "<config-version>%s</config-version>"
        "<resources>%s%s</resources>"
        "<logical-switches>%s</logical-switches></capable-switch>";

    char *ports;
    char *flow_tables;
    char *bridges;

    struct ds state_data;

    if (ovsdb_handler == NULL) {
        return NULL;
    }
    ofconf_update(ovsdb_handler);

    ports = get_ports_state();
    if (ports == (NULL)) {
        ports = strdup("");
    }
    flow_tables = get_flow_tables_state();
    if (flow_tables == (NULL)) {
        flow_tables = strdup("");
    }
    bridges = get_bridges_state();
    if (bridges == (NULL)) {
        bridges = strdup("");
    }

    ds_init(&state_data);

    ds_put_format(&state_data, state_data_format, "1.2", ports, flow_tables,
                  bridges);

    free(ports);
    free(flow_tables);
    free(bridges);

    return ds_steal_cstr(&state_data);
}

bool
ofconf_init(const char *ovs_db_path)
{
    ovsdb_t *p = calloc(1, sizeof *p);

    if (p == NULL) {
        /* failed */
        return false;
    }
    /* create new resource-id map of 1024 elements, it will grow when needed */
    p->resource_map = ofc_resmap_init(1024);
    if (p->resource_map == NULL) {
        free(p);
        return false;
    }

    ovsrec_init();
    p->idl = ovsdb_idl_create(ovs_db_path, &ovsrec_idl_class, true, true);
    p->seqno = ovsdb_idl_get_seqno(p->idl);
    ofconf_update(p);
    ovsdb_handler = p;

    /* prepare descriptor to perform ioctl() */
    ioctlfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);

    return true;
}

void
ofconf_destroy(void)
{
    if (ovsdb_handler != NULL) {
        /* close everything */
        ovsdb_idl_destroy(ovsdb_handler->idl);

        ofc_resmap_destroy(&ovsdb_handler->resource_map);

        free(ovsdb_handler);
        ovsdb_handler = NULL;
    }

    if (ioctlfd != -1) {
        close(ioctlfd);
        ioctlfd = -1;
    }
}

/* Notes:
OpenFlow access:
    +--rw capable-switch
        +--rw resources
            +--rw port* [resource-id]
                +--rw configuration
                    no-receive
                    no-forward
                    no-packet-in
                +--ro state
                    +--ro live?
        The no-receive is true if NO_RECV is found.
        The no-forward is true if NO_FWD is found.
        The no-packet-in is true if NO_PACKET_IN is found.
        Use ovs-ofctl(8) to set values:
        # ovs-ofctl mod-port <SWITCH> <PORT> <no-receive|receive>
        # ovs-ofctl mod-port <SWITCH> <PORT> <no-forward|forward>
        # ovs-ofctl mod-port <SWITCH> <PORT> <no-packet-in|packet-in>
        # ovs-ofctl show <SWITCH>
        Note: The value is true if LIVE is found on the line:
        state: ...

OVSDB access:
+--rw capable-switch
    +--rw id - internally
    +--ro config-version? 1.2
    +--rw resources
        +--rw port* [resource-id]
            +--rw resource-id
            +--ro number?           ovsrec_interface->ofport[n_ofport]
            +--rw requested-number? ovsrec_interface->ofport_request[n_ofport_request]
            +--ro name?             ovsrec_interface->name
            +--ro state
                +--ro oper-state?   ovsrec_interface->link_state
                +--ro blocked?      ovsrec_interface->status:stp_state
            +--rw (tunnel-type)?
                +--:(tunnel)
                    +--rw tunnel
                        +--rw (endpoints)
                            +--:(v4-endpoints)
                                +--rw local-endpoint-ipv4-adress?      ovsrec_interface->options:local_ip
                                +--rw remote-endpoint-ipv4-adress?     ovsrec_interface->options:remote_ip
                            +--:(ipgre-tunnel)
                                +--rw ipgre-tunnel
                                +--rw (endpoints)
                                | +--:(v4-endpoints)
                                |    +--rw local-endpoint-ipv4-adress?      ovsrec_interface->options:local_ip
                                |    +--rw remote-endpoint-ipv4-adress?     ovsrec_interface->options:remote_ip
                                +--rw checksum-present?                     ovsrec_interface->options:csum
                                +--rw key-present?
                                +--rw key                                   ovsrec_interface->options:key
                            +--:(vxlan-tunnel)
                                +--rw vxlan-tunnel
                                +--rw (endpoints)
                                | +--:(v4-endpoints)
                                |    +--rw local-endpoint-ipv4-adress?      ovsrec_interface->options:local_ip
                                |    +--rw remote-endpoint-ipv4-adress?     ovsrec_interface->options:remote_ip
                                +--rw vni?                                  ovsrec_interface->options:key
        +--rw queue* [resource-id]
            +--rw resource-id
            +--rw id
            +--rw port?
            +--rw properties
                +--rw min-rate?         ovsrec queue->other_config:min-rate
                +--rw max-rate?         ovsrec queue->other_config:max-rate
                +--rw experimenter-id?
                +--rw experimenter-data?

ioctl access:
+--rw capable-switch
    +--rw id - internally
    +--ro config-version? 1.2
    +--rw resources
        +--rw port* [resource-id]
            +--ro current-rate?
            +--rw configuration
                +--rw admin-state
*/

