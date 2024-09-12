/*
 * Copyright (c) 2023 Cisco and/or its affiliates.
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

#include "SwitchStateBase.h"

#include "swss/logger.h"
#include "swss/exec.h"
#include "swss/converter.h"

#include "meta/sai_serialize.h"
#include "meta/NotificationPortStateChange.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>

#include "SwitchStateBaseUtils.h"
#include "vppxlate/SaiVppXlate.h"
#include "vppxlate/SaiAclStats.h"

#include <list>

using namespace saivpp;


static sai_status_t acl_ip_field_to_vpp_acl(
    _In_ sai_acl_entry_attr_t         attr_id,
    _In_ const sai_attribute_value_t *value,
    _Out_ vpp_acl_rule_t *rule)
{
    sai_ip_addr_family_t                    addr_family;

    assert((SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6 == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6 == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_DST_IP == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IP == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IPV6 == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IPV6 == attr_id));

    if (!value->aclfield.enable) {
        SWSS_LOG_NOTICE("aclfield not enabled for ip prefix");
        return SAI_STATUS_SUCCESS;
    }

    vpp_ip_addr_t *ip_addr, *ip_mask;

    switch (attr_id) {
    case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP:
	ip_addr = &rule->src_prefix;
	ip_mask = &rule->src_prefix_mask;
        addr_family = SAI_IP_ADDR_FAMILY_IPV4;
	break;

    case SAI_ACL_ENTRY_ATTR_FIELD_DST_IP:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IP:
	ip_addr = &rule->dst_prefix;
	ip_mask = &rule->dst_prefix_mask;
        addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        break;

    case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IPV6:
	ip_addr = &rule->src_prefix;
	ip_mask = &rule->src_prefix_mask;
        addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        break;

    case SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IPV6:
	ip_addr = &rule->dst_prefix;
	ip_mask = &rule->dst_prefix_mask;
        addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        break;

    default:
        SWSS_LOG_ERROR("Unexpected ip field type (%u)\n", attr_id);
        return SAI_STATUS_FAILURE;
    }

    if (SAI_IP_ADDR_FAMILY_IPV4 == addr_family) {
	struct sockaddr_in *sin =  &ip_addr->addr.ip4;

	ip_addr->sa_family = AF_INET;
	sin->sin_addr.s_addr = value->aclfield.data.ip4;

	sin =  &ip_mask->addr.ip4;
	sin->sin_addr.s_addr = value->aclfield.mask.ip4;
	SWSS_LOG_NOTICE("Setting ipv4 subnet %x %x", value->aclfield.data.ip4,
			value->aclfield.mask.ip4);
    } else {
	struct sockaddr_in6 *sin6 =  &ip_addr->addr.ip6;

	ip_addr->sa_family = AF_INET6;
	memcpy(sin6->sin6_addr.s6_addr, value->aclfield.data.ip6, sizeof(sin6->sin6_addr.s6_addr));

	sin6 =  &ip_mask->addr.ip6;
        memcpy(sin6->sin6_addr.s6_addr, &value->aclfield.mask.ip6, sizeof(value->aclfield.mask.ip6));
    }

    return SAI_STATUS_SUCCESS;
}

static void set_ipv4any_addr_mask (vpp_ip_addr_t *ip_addr)
{
    struct sockaddr_in *sin =  &ip_addr->addr.ip4;

    ip_addr->sa_family = AF_INET;
    sin->sin_addr.s_addr = (uint32_t) 0;
}

static void set_ipv6any_addr_mask (vpp_ip_addr_t *ip_addr)
{
    struct sockaddr_in6 *sin6 =  &ip_addr->addr.ip6;

    ip_addr->sa_family = AF_INET6;

    unsigned long long v6_mask = (unsigned long long) 0;

    memcpy(sin6->sin6_addr.s6_addr, &v6_mask, 8);
    memcpy(&sin6->sin6_addr.s6_addr[8], &v6_mask, 8);
}

static sai_status_t acl_ip_type_field_to_vpp_acl_rule(
    _In_ sai_acl_entry_attr_t         attr_id,
    _In_ const sai_attribute_value_t *value,
    _Out_ vpp_acl_rule_t *rule)
{
    sai_acl_ip_type_t ip_type;

    assert(SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE == attr_id);

    if (!value->aclfield.enable) {
        return SAI_STATUS_SUCCESS;
    }

    ip_type = (sai_acl_ip_type_t) value->aclfield.data.s32;

    switch (ip_type) {
    case SAI_ACL_IP_TYPE_ANY:
        /* Do nothing */
        break;

    case SAI_ACL_IP_TYPE_IP:
	/* Do nothing for now */
        break;

    case SAI_ACL_IP_TYPE_IPV4ANY:
	set_ipv4any_addr_mask(&rule->src_prefix);
	set_ipv4any_addr_mask(&rule->dst_prefix);
	set_ipv4any_addr_mask(&rule->src_prefix_mask);
	set_ipv4any_addr_mask(&rule->dst_prefix_mask);

        break;

    case SAI_ACL_IP_TYPE_IPV6ANY:
	set_ipv6any_addr_mask(&rule->src_prefix);
	set_ipv6any_addr_mask(&rule->dst_prefix);
	set_ipv6any_addr_mask(&rule->src_prefix_mask);
	set_ipv6any_addr_mask(&rule->dst_prefix_mask);

        break;

    default:
        SWSS_LOG_NOTICE("Unsupported ip type (%d)\n", ip_type);
	return SAI_STATUS_SUCCESS;
    }
    return SAI_STATUS_SUCCESS;
}

static sai_status_t acl_icmp_field_to_vpp_acl_rule(
    _In_ sai_acl_entry_attr_t          attr_id,
    _In_ const sai_attribute_value_t  *value,
    _Out_ vpp_acl_rule_t *rule)
{
    uint16_t                                data = 0, mask = 0;
    uint16_t                                new_data, new_mask;

    assert((SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE == attr_id) ||
           (SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE == attr_id));

    new_data = (value->aclfield.enable) ? value->aclfield.data.u8 : 0;
    new_mask = (value->aclfield.enable) ? value->aclfield.mask.u8 : 0;


    switch (attr_id) {
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE:
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE:
        data = (uint16_t) ((data & 0xFF) | (new_data << 8));
        mask = (uint16_t) ((mask & 0xFF) | (new_mask << 8));

	rule->dstport_or_icmpcode_first = data;
	rule->srcport_or_icmptype_last = mask;

        break;

    case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE:
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE:
        data = (uint16_t) ((data & 0xFF00) | new_data);
        mask = (uint16_t) ((mask & 0xFF00) | new_mask);

	rule->srcport_or_icmptype_first = data;
	rule->srcport_or_icmptype_last = mask;

        break;

    default:
        SWSS_LOG_ERROR("Unexpected attr_id %d\n", attr_id);
        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t acl_entry_port_to_vpp_acl_rule(
    _In_ sai_acl_entry_attr_t          attr_id,
    _In_ const sai_attribute_value_t  *value,
    _Out_ vpp_acl_rule_t      *rule)
{
    if (!value->aclfield.enable) {
	SWSS_LOG_NOTICE("aclfield disabled for port configuration");
	return SAI_STATUS_SUCCESS;
    }

    switch (attr_id) {
    case SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT:
	rule->srcport_or_icmptype_first = value->aclfield.data.u16;
	rule->srcport_or_icmptype_last = value->aclfield.data.u16;
	break;

    case SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT:
	rule->dstport_or_icmpcode_first = value->aclfield.data.u16;
	rule->dstport_or_icmpcode_last = value->aclfield.data.u16;
	break;

    default:
	break;
    }
    return SAI_STATUS_SUCCESS;
}

static sai_status_t acl_rule_port_range_vpp_acl_set(
    _In_ sai_acl_range_type_t     type,
    _In_ const sai_u32_range_t   *range,
    _Out_ vpp_acl_rule_t *rule)
{
    assert(range);

    switch (type) {
    case SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE:
	rule->srcport_or_icmptype_first = (uint16_t) range->min;
	rule->srcport_or_icmptype_last = (uint16_t) range->max;
	SWSS_LOG_NOTICE("SRC port range %u-%u", range->min, range->max);
        break;

    case SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE:
	rule->dstport_or_icmpcode_first = (uint16_t) range->min;
	rule->dstport_or_icmpcode_last = (uint16_t) range->max;
	SWSS_LOG_NOTICE("DST port range %u-%u", range->min, range->max);
        break;

    default:
        SWSS_LOG_NOTICE("Range type %d is not supported\n", type);
	break;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::acl_rule_range_get(
    _In_ const sai_object_list_t   *range_list,
    _Out_ sai_u32_range_t *range_limit_list,
    _Out_ sai_acl_range_type_t *range_type_list,
    _Out_ uint32_t *range_count)
{
    uint32_t idx, count = 0;

    sai_u32_range_t   *range = range_limit_list;
    sai_acl_range_type_t *range_type = range_type_list;

    for (idx = 0; idx < range_list->count; idx++) {
	sai_object_id_t oid;

	oid = range_list->list[idx];

	if (SAI_OBJECT_TYPE_ACL_RANGE == sai_object_type_query(oid)) {
	    sai_attribute_t attr;

	    attr.id = SAI_ACL_RANGE_ATTR_TYPE;
	    if (get(SAI_OBJECT_TYPE_ACL_RANGE, oid, 1, &attr) == SAI_STATUS_SUCCESS) {
		sai_acl_range_type_t     type;

		type = (sai_acl_range_type_t) attr.value.s32;
		attr.id = SAI_ACL_RANGE_ATTR_LIMIT;
		if (get(SAI_OBJECT_TYPE_ACL_RANGE, oid, 1, &attr) == SAI_STATUS_SUCCESS) {

		    *range = attr.value.u32range;
		    *range_type = type;

		    range++;
		    range_type++;
		    count++;

		    if (count == 2) break;
		}
	    } else {
		SWSS_LOG_ERROR("SAI_OBJECT_TYPE_ACL_RANGE not found for ACL_RANGE oid");
		return SAI_STATUS_FAILURE;
	    }
	}
    }

    *range_count = count;

    return SAI_STATUS_SUCCESS;
}

static void acl_rule_set_action(
    _In_ const sai_attribute_value_t  *value,
    _Out_ vpp_acl_rule_t      *rule)

{
    	switch (value->aclaction.parameter.s32) {
	case SAI_PACKET_ACTION_FORWARD:
	    rule->action = VPP_ACL_ACTION_API_PERMIT_STFULL;
	    break;

	case SAI_PACKET_ACTION_DROP:
	    rule->action = VPP_ACL_ACTION_API_DENY;
	    break;
	}
}

sai_status_t acl_rule_field_update(
    _In_ sai_acl_entry_attr_t          attr_id,
    _In_ const sai_attribute_value_t  *value,
    _Out_ vpp_acl_rule_t      *rule)
{
    sai_status_t status;

    assert(NULL != value);
    status = SAI_STATUS_SUCCESS;

    switch (attr_id) {
    case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP:
    case SAI_ACL_ENTRY_ATTR_FIELD_DST_IP:
    case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6:
    case SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IP:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IPV6:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IPV6:
        status = acl_ip_field_to_vpp_acl(attr_id, value, rule);
        break;

    case SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE:
	status = acl_ip_type_field_to_vpp_acl_rule(attr_id, value, rule);
        break;


    case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE:
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE:
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE:
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE:
        status = acl_icmp_field_to_vpp_acl_rule(attr_id,
						value, rule);
        break;

    case SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT:
    case SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT:
	status = acl_entry_port_to_vpp_acl_rule(attr_id, value, rule);
	break;

    case SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL:
	rule->proto = value->aclfield.data.u8 & value->aclfield.mask.u8;
	status = SAI_STATUS_SUCCESS;
	break;

    case SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION:
	acl_rule_set_action(value, rule);
	break;

    default:
        break;
    }

    return status;
}

sai_status_t SwitchStateBase::tunterm_set_action_redirect(
    _In_ sai_acl_entry_attr_t          attr_id,
    _In_ const sai_attribute_value_t  *value,
    _Out_ tunterm_acl_rule_t      *rule)
{
    sai_status_t                 status = SAI_STATUS_SUCCESS;
    sai_object_id_t              next_hop_oid;
    sai_object_id_t              rif_oid;
    sai_object_id_t              port_oid;
    sai_ip_address_t             ip_address;
    uint32_t                     next_hop_type;

    next_hop_oid = value->aclaction.parameter.oid;

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr));
    next_hop_type = attr.value.s32;
    if (next_hop_type!= SAI_NEXT_HOP_TYPE_IP) {
	    return SAI_STATUS_SUCCESS;
    }
    attr.id = SAI_NEXT_HOP_ATTR_IP;
    if (get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr) == SAI_STATUS_SUCCESS)
    {
	    ip_address = attr.value.ipaddr;
    } else {
        SWSS_LOG_ERROR("IP address missing in nexthop %s", sai_serialize_object_id(next_hop_oid).c_str());
        return SAI_STATUS_SUCCESS;
    }
    
    attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    if (get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr) == SAI_STATUS_SUCCESS)
    {
        rif_oid = attr.value.oid;
    } else {
        SWSS_LOG_ERROR("RIF missing in nexthop %s", sai_serialize_object_id(next_hop_oid).c_str());
        return SAI_STATUS_SUCCESS;
    }

    attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
    if (get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_oid, 1, &attr) == SAI_STATUS_SUCCESS)
    {
        port_oid = attr.value.oid;
    } else {
        SWSS_LOG_ERROR("Port ID is missing missing in RIF object %s", sai_serialize_object_id(rif_oid).c_str());
        return SAI_STATUS_SUCCESS;
    }

    std::string hwif_name;
	if (!vpp_get_hwif_name(port_oid, 0, hwif_name)) {
	    SWSS_LOG_WARN("VPP hwif name not found for port %s", sai_serialize_object_id(port_oid).c_str());
        return SAI_STATUS_SUCCESS;
	}

    switch (ip_address.addr_family) {
        case SAI_IP_ADDR_FAMILY_IPV4:
        {
            vpp_ip_addr_t *ip_addr;
            rule->ip_protocol = 1; // IP46_TYPE_IP4=1, IP46_TYPE_IP6=2
            ip_addr = &rule->next_hop_ip;
            struct sockaddr_in *sin =  &ip_addr->addr.ip4;
            ip_addr->sa_family = AF_INET;
            sin->sin_addr.s_addr = ip_address.addr.ip4;
            break;
        }
        case SAI_IP_ADDR_FAMILY_IPV6:
        {
            vpp_ip_addr_t *ip_addr;
            rule->ip_protocol = 2; // IP46_TYPE_IP4=1, IP46_TYPE_IP6=2            
            ip_addr = &rule->next_hop_ip;
            struct sockaddr_in6 *sin6 =  &ip_addr->addr.ip6;
            ip_addr->sa_family = AF_INET6;
            memcpy(sin6->sin6_addr.s6_addr, &ip_address.addr.ip6, sizeof(sin6->sin6_addr.s6_addr));
            break;
        }
        default:
            break;
    }

    strncpy(rule->hwif_name, hwif_name.c_str(), sizeof(rule->hwif_name) -1);

    SWSS_LOG_NOTICE("Tunterm rule received: IP Protocol %d, rif_oid %d, next-hop hwif_name %s", rule->ip_protocol,
                    rif_oid, rule->hwif_name);
    
    if(rule->ip_protocol == 1) {
        SWSS_LOG_NOTICE("Tunterm acl rule has next-hop IP %x", rule->next_hop_ip.addr.ip4.sin_addr.s_addr);
    } else if (rule->ip_protocol == 2) {
        for (int i=0; i<16; i++) {
            SWSS_LOG_NOTICE("Tunterm acl rule has next-hop IP6 %u", rule->next_hop_ip.addr.ip6.sin6_addr.s6_addr[i]);
        }
        
    }
    

    return status;


}

sai_status_t SwitchStateBase::tunterm_acl_rule_field_update(
    _In_ sai_acl_entry_attr_t          attr_id,
    _In_ const sai_attribute_value_t  *value,
    _Out_ tunterm_acl_rule_t      *rule)
{
    sai_status_t status;

    assert(NULL != value);
    status = SAI_STATUS_SUCCESS;

    switch (attr_id) {
    case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP:
        break;
    case SAI_ACL_ENTRY_ATTR_FIELD_DST_IP:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IP:
    {
        vpp_ip_addr_t *ip_addr, *ip_mask;
        ip_addr = &rule->dst_prefix;
	    ip_mask = &rule->dst_prefix_mask;
        struct sockaddr_in *sin =  &ip_addr->addr.ip4;
        ip_addr->sa_family = AF_INET;
        sin->sin_addr.s_addr = value->aclfield.data.ip4;
        sin =  &ip_mask->addr.ip4;
        sin->sin_addr.s_addr = value->aclfield.mask.ip4;
        SWSS_LOG_NOTICE("Tunterm acl rule has dst IP: %x", sin->sin_addr.s_addr);
        break;
    }
    case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IPV6:
        break;
    case SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6:
    case SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IPV6:
    {
        vpp_ip_addr_t *ip_addr, *ip_mask;
        ip_addr = &rule->dst_prefix;
	    ip_mask = &rule->dst_prefix_mask;
        struct sockaddr_in6 *sin6 =  &ip_addr->addr.ip6;
        ip_addr->sa_family = AF_INET6;
        memcpy(sin6->sin6_addr.s6_addr, value->aclfield.data.ip6, sizeof(sin6->sin6_addr.s6_addr));
        for (int i=0; i<16; i++) {
            SWSS_LOG_NOTICE("Tunterm acl rule has dst IP6:  %u", sin6->sin6_addr.s6_addr[i]);
        }
	    sin6 =  &ip_mask->addr.ip6;
        memcpy(sin6->sin6_addr.s6_addr, &value->aclfield.mask.ip6, sizeof(value->aclfield.mask.ip6));
        
        break;
    }
    case SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE:
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE:
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE:
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE:
    case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE:
    case SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT:
    case SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT:
    case SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL:
    case SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION:
        break;
    case SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT:
        status = tunterm_set_action_redirect(attr_id, value, rule);
	    break;
    default:
        break;
    }

    return status;
}

sai_status_t SwitchStateBase::getAclTableId(
    _In_ sai_object_id_t entry_id, sai_object_id_t *tbl_oid)
{
    sai_attribute_t attr;

    attr.id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    if (get(SAI_OBJECT_TYPE_ACL_ENTRY, entry_id, 1, &attr) != SAI_STATUS_SUCCESS) {
	auto sid = sai_serialize_object_id(entry_id);

	SWSS_LOG_ERROR("ACL table for acl entry id %s not found", sid.c_str());
	return SAI_STATUS_FAILURE;
    }

    *tbl_oid = attr.value.oid;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::acl_range_attr_get (
    _In_ const std::string &serializedObjectId,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list,
    _Out_ sai_attribute_t *attr_range)
{
    const sai_attribute_t *attr;

    for (uint32_t i = 0; i < attr_count; i++) {
	attr = &attr_list[i];
	if (attr->id == SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE) {
	    attr_range->id = SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE;
	    return get(SAI_OBJECT_TYPE_ACL_ENTRY, serializedObjectId,
		       1, attr_range);
	}
    }

    return SAI_STATUS_FAILURE;
}

sai_status_t acl_priority_attr_get (
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list,
    uint32_t *priority)
{
    const sai_attribute_t *attr;

    for (uint32_t i = 0; i < attr_count; i++) {
	attr = &attr_list[i];
	if (attr->id == SAI_ACL_ENTRY_ATTR_PRIORITY) {
	    *priority = attr->value.u32;
	    return SAI_STATUS_SUCCESS;
	}
    }

    return SAI_STATUS_FAILURE;
}

#define MAX_ACL_ATTRS 12

typedef struct _acl_tbl_entries_ {
    uint32_t priority;

    sai_attribute_t  attr_range;
    sai_object_id_t  range_objid_list[2];

    sai_u32_range_t range_limit[2];
    sai_acl_range_type_t range_type[2];
    uint32_t range_count;
    sai_attribute_t attrs[MAX_ACL_ATTRS];
    uint32_t attrs_count;
} acl_tbl_entries_t;

typedef struct ordered_ace_list_ {
    uint32_t index;
    uint32_t priority;
    sai_object_id_t ace_oid;
} ordered_ace_list_t;

static bool cmp_priority (
    const ordered_ace_list_t& f,
    const ordered_ace_list_t& s)
{
    return (f.priority > s.priority);
}

sai_status_t SwitchStateBase::tunterm_acl_bindunbind(sai_object_id_t tbl_oid, bool is_add, std::string hwif_name)
{
    auto tunterm_idx_it = m_tunterm_acl_swindex_map.find(tbl_oid);
    if (tunterm_idx_it != m_tunterm_acl_swindex_map.end()) {
        auto tunterm_idx = tunterm_idx_it->second;
        vpp_tunterm_acl_interface_add_del(tunterm_idx, is_add, hwif_name.c_str());
    }
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::tunterm_acl_add_replace(vpp_tunerm_acl_t *acl, sai_object_id_t tbl_oid)
{
    sai_status_t status = SAI_STATUS_SUCCESS;
    uint32_t tunterm_acl_swindex = 0;
    bool do_port_bind = false;

    auto tunterm_idx_it = m_tunterm_acl_swindex_map.find(tbl_oid);
    if ((tunterm_idx_it == m_tunterm_acl_swindex_map.end()) && (acl->count > 0)) {
        tunterm_acl_swindex = ~tunterm_acl_swindex;
        do_port_bind = true;
    } else if (tunterm_idx_it != m_tunterm_acl_swindex_map.end()) {
        tunterm_acl_swindex = tunterm_idx_it->second;
    } else {
        SWSS_LOG_NOTICE("No tunterm acl rules to add.");
        return status;
    }

    std::list<std::string> hwif_names;
    SWSS_LOG_NOTICE("Adding tunterm acl rules, tunterm_acl index %u", tunterm_acl_swindex);

    status = vpp_tunterm_acl_add_replace(&tunterm_acl_swindex, acl->count, acl);

    if (status == SAI_STATUS_SUCCESS) {
        m_tunterm_acl_swindex_map[tbl_oid] = tunterm_acl_swindex;
    } else {
        return SAI_STATUS_FAILURE;
    }

    if(do_port_bind) {
        auto it_hw_ports = m_acl_tbl_hw_ports_map.find(tbl_oid);
        if (it_hw_ports == m_acl_tbl_hw_ports_map.end()) {
            auto sid = sai_serialize_object_id(tbl_oid);
            SWSS_LOG_WARN("Tunterm acl - no ports bound for table id %s", sid.c_str());
        } else {
            hwif_names = it_hw_ports->second;
            for (auto hwif_name: hwif_names) {
                SWSS_LOG_NOTICE("Tunterm acl - binding to hwif %s", hwif_name.c_str());
                tunterm_acl_bindunbind(tbl_oid, true, hwif_name);
            }
        }
    }
    
    return status;
}

sai_status_t SwitchStateBase::tbl_hw_ports_map_delete(sai_object_id_t tbl_oid)
{
    auto hw_ports_it = m_acl_tbl_hw_ports_map.find(tbl_oid);
    if (hw_ports_it == m_acl_tbl_hw_ports_map.end()) {
        return SAI_STATUS_SUCCESS;
    }
    m_acl_tbl_hw_ports_map.erase(hw_ports_it);
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::tunterm_acl_delete(sai_object_id_t tbl_oid)
{
    sai_status_t status = SAI_STATUS_SUCCESS;

    auto tunterm_idx_it = m_tunterm_acl_swindex_map.find(tbl_oid);
    if (tunterm_idx_it == m_tunterm_acl_swindex_map.end()) {
        SWSS_LOG_WARN("No tunterm ACL configured for table %s", sai_serialize_object_id(tbl_oid).c_str());
        return SAI_STATUS_SUCCESS;
    }
    uint32_t tunterm_acl_swindex = tunterm_idx_it->second;

    status = vpp_tunterm_acl_del(tunterm_acl_swindex);

    if (status == SAI_STATUS_SUCCESS) {
	    m_tunterm_acl_swindex_map.erase(tunterm_idx_it);
        tbl_hw_ports_map_delete(tbl_oid);
    }
    SWSS_LOG_NOTICE("Tunterm acl table %s, remove tunterm_acl index %u, status %d",
		    sai_serialize_object_id(tbl_oid).c_str(), tunterm_acl_swindex, status);

    return status;
}

sai_status_t SwitchStateBase::AclTblConfig(
    _In_ sai_object_id_t tbl_oid)
{
    auto it = m_acl_tbl_rules_map.find(tbl_oid);

    sai_status_t status = SAI_STATUS_SUCCESS;

    if (it == m_acl_tbl_rules_map.end()) {
	auto sid = sai_serialize_object_id(tbl_oid);
	SWSS_LOG_WARN("No ACL entry list for table id %s", sid.c_str());
	return SAI_STATUS_FAILURE;
    }
    std::list<sai_object_id_t>& acl_entries = it->second;
    size_t n_total_entries = acl_entries.size();

    if (n_total_entries == 0) {
	return SAI_STATUS_SUCCESS;
    }

    acl_tbl_entries_t *aces, *p_ace;
    std::list<ordered_ace_list_t> ordered_aces = {};
    uint32_t index;

    aces = (acl_tbl_entries_t *) calloc(n_total_entries, sizeof(acl_tbl_entries_t));
    if (!aces) {
	return SAI_STATUS_FAILURE;
    }
    p_ace = aces;

    index = 0;

    /* Collect ACL entries configuration */
    for (auto entry_id: acl_entries) {
	SWSS_LOG_NOTICE("Processing ACL entry %s", sai_serialize_object_id(entry_id).c_str());

	auto sid = sai_serialize_object_id(entry_id);

	if (get(SAI_OBJECT_TYPE_ACL_ENTRY, sid, MAX_ACL_ATTRS,
		&p_ace->attrs_count, p_ace->attrs) != SAI_STATUS_SUCCESS) {
	    status = SAI_STATUS_FAILURE;
	    break;
	}
	p_ace->attr_range.value.aclfield.data.objlist.list = p_ace->range_objid_list;
	p_ace->attr_range.value.aclfield.data.objlist.count = 2;

	if (acl_range_attr_get(sid, p_ace->attrs_count,
			       p_ace->attrs, &p_ace->attr_range) == SAI_STATUS_SUCCESS) {
	    p_ace->range_count = 0;
	    if (acl_rule_range_get(&p_ace->attr_range.value.aclfield.data.objlist,
				   p_ace->range_limit, p_ace->range_type,
				   &p_ace->range_count) != SAI_STATUS_SUCCESS) {
		status = SAI_STATUS_FAILURE;
		break;
	    }
	}

	p_ace->priority = 0;
	acl_priority_attr_get(p_ace->attrs_count, p_ace->attrs, &p_ace->priority);

	ordered_aces.push_back({index, p_ace->priority, entry_id});
	p_ace++;
	index++;
    }

    if (status != SAI_STATUS_SUCCESS) {
	free(aces);
	ordered_aces.clear();
	return SAI_STATUS_FAILURE;
    }

    /* Sort ACL entries on priority */
    ordered_aces.sort(cmp_priority);
    // SWSS_LOG_NOTICE("# aces %u after sort", ordered_aces.size());

    size_t n_entries = 0;
    size_t n_tunterm_entries  = 0;
    for (auto ace: ordered_aces) {
        p_ace = &aces[ace.index];
        const sai_attribute_t *attr;
        bool ttf_flag_set = false;
        for (uint32_t i = 0; i < p_ace->attrs_count; i++) {
            attr = &p_ace->attrs[i];
            if(attr->id == SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_TERMINATED) {
                ttf_flag_set=true;
                break;
            }
        }
        if(ttf_flag_set) { 
            n_tunterm_entries++;
        } else {
            n_entries++;
        }        
    }

    SWSS_LOG_NOTICE("Tunterm acl count: %d, regular acl count: %d, total: %d", n_tunterm_entries, n_entries, n_total_entries);

    vpp_acl_t *acl;

    acl = (vpp_acl_t *) calloc(1, sizeof(vpp_acl_t) + (n_entries * sizeof(vpp_acl_rule_t)));
    if (!acl) {
	free(aces);
	ordered_aces.clear();
	return SAI_STATUS_FAILURE;
    }
    acl->count = (uint32_t) n_entries;
    char aclname[64];

    auto tbl_sid = sai_serialize_object_id(tbl_oid);
    snprintf(aclname, sizeof(aclname), "sonic_acl_%s", tbl_sid.c_str());
    acl->acl_name = aclname;

    vpp_acl_rule_t *rule = &acl->rules[0];

    /* Tunterm ACL setup */
    vpp_tunerm_acl_t *tunterm_acl;
    tunterm_acl = (vpp_tunerm_acl_t *) calloc(1, sizeof(vpp_tunerm_acl_t) + (n_tunterm_entries * sizeof(tunterm_acl_rule_t)));
    if (!tunterm_acl) {
        free(aces);
        free(acl);
        ordered_aces.clear();
        return SAI_STATUS_FAILURE;
    }
    tunterm_acl->count = (uint32_t) n_tunterm_entries;
    char tunterm_aclname[64];
    snprintf(tunterm_aclname, sizeof(tunterm_aclname), "tunterm_sonic_acl_%s", tbl_sid.c_str());
    tunterm_acl->acl_name = tunterm_aclname;
    tunterm_acl_rule_t *tunterm_rule = &tunterm_acl->rules[0];

    std::map<sai_object_id_t, uint32_t> acl_aces_index_map;
    for (auto ace: ordered_aces) {
	SWSS_LOG_NOTICE("Acl entry index %u prtiority %u", ace.index, ace.priority);

	p_ace = &aces[ace.index];
	const sai_attribute_t *attr;
    bool update_tunterm_rule = false;
	for (uint32_t i = 0; i < p_ace->attrs_count; i++) {
	    attr = &p_ace->attrs[i];
	    auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_ENTRY, attr->id);

	    if (meta != NULL) {
	        SWSS_LOG_NOTICE("Type %s attrib id %s",
				sai_serialize_object_type(SAI_OBJECT_TYPE_ACL_ENTRY).c_str(),
				meta->attridname);
	    }
	    
        if(attr->id == SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_TERMINATED) {
            update_tunterm_rule = true;
        }

	    if (attr->id == SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE && acl->count>0  && (rule<(&acl->rules[0]+n_entries))) {
		for (uint32_t jdx = 0; jdx < p_ace->range_count; jdx++) {
		    status = acl_rule_port_range_vpp_acl_set(p_ace->range_type[jdx],
							     &p_ace->range_limit[jdx], rule);
		}
	    } else {
            if(acl->count>0 && (rule<(&acl->rules[0]+n_entries))) {
		        status = acl_rule_field_update((sai_acl_entry_attr_t) attr->id, &attr->value, rule);
            }
            if(tunterm_acl->count>0 && (tunterm_rule<(&tunterm_acl->rules[0]+n_tunterm_entries))) {
                status = tunterm_acl_rule_field_update((sai_acl_entry_attr_t) attr->id, &attr->value, tunterm_rule);
	        }
        }
	}
    
    if(update_tunterm_rule) {
        if(acl->count>0 && (rule<(&acl->rules[0]+n_entries))) {
            memset(rule, 0, sizeof(*rule)); // clear any fields that were added to regular rule
        }
        tunterm_rule++;
    } else {
        if(tunterm_acl->count>0 && (tunterm_rule<(&tunterm_acl->rules[0]+n_tunterm_entries))) {
            memset(tunterm_rule, 0, sizeof(*tunterm_rule)); // clear any fields that were added to tunterm_rule
	    }
        rule++;
    } 
    }

    bool acl_replace;
    uint32_t acl_swindex;
    auto vpp_idx_it = m_acl_swindex_map.find(tbl_oid);
    if(acl->count > 0) {
        if (vpp_idx_it == m_acl_swindex_map.end()) {
        acl_swindex = 0;
        acl_replace = false;
        } else {
        acl_swindex = vpp_idx_it->second;
        acl_replace = true;
        }

        status = vpp_acl_add_replace(acl, &acl_swindex, acl_replace);
        if (status == SAI_STATUS_SUCCESS) {
        m_acl_swindex_map[tbl_oid] = acl_swindex;

        index = 0;

        for (auto ace: ordered_aces) {
            p_ace = &aces[ace.index];
            const sai_attribute_t *attr;
            sai_object_id_t ace_cntr_oid;

            for (uint32_t i = 0; i < p_ace->attrs_count; i++) {
            attr = &p_ace->attrs[i];

            if (attr->id == SAI_ACL_ENTRY_ATTR_ACTION_COUNTER) {
                ace_cntr_oid = attr->value.aclaction.parameter.oid;

                auto ace_it = m_ace_cntr_info_map.find(ace_cntr_oid);

                if (ace_it != m_ace_cntr_info_map.end()) {
                m_ace_cntr_info_map.erase(ace_it);
                }
        
                // For stats we need to find vpp rule index from acl_entry_counter (ace_counter)
                m_ace_cntr_info_map[ace_cntr_oid] = { tbl_oid, ace.ace_oid, acl_swindex, index };
            }
            }
            index++;
        }
        }
    }

    SWSS_LOG_NOTICE("ACL table %s %s status %d", tbl_sid.c_str(),
		    acl_replace ? "replace" : "add", status);
    
    if(status == SAI_STATUS_SUCCESS) {
        status = tunterm_acl_add_replace(tunterm_acl, tbl_oid);
        SWSS_LOG_NOTICE("Tunterm ACL table %s %s status %d", tbl_sid.c_str(),
		    acl_replace ? "replace" : "add", status);
    } else {
        SWSS_LOG_NOTICE("ACL plugin operation failed, skipping tunterm configuration.");;
    }

    free(aces);
    free(acl);
    ordered_aces.clear();
    
    free(tunterm_acl);

    return status;
}

sai_status_t SwitchStateBase::aclGetVppIndices(
    _In_ sai_object_id_t ace_cntr_oid,
    _Out_ uint32_t *acl_index,
    _Out_ uint32_t *ace_index)
{
    auto vpp_ace_it = m_ace_cntr_info_map.find(ace_cntr_oid);
    if (vpp_ace_it == m_ace_cntr_info_map.end()) {
	SWSS_LOG_WARN("VPP ace entry %s not found in vpp_ace_cntr_info_map",
		      sai_serialize_object_id(ace_cntr_oid).c_str());
	return SAI_STATUS_FAILURE;
    }
    auto & ace_info = vpp_ace_it->second;

    *acl_index = ace_info.acl_index;
    *ace_index = ace_info.ace_index;

    SWSS_LOG_NOTICE("VPP acl index %u ace index %u for acl_counter %s acl_table %s",
		    *acl_index, *ace_index,
		    sai_serialize_object_id(ace_cntr_oid).c_str(),
		    sai_serialize_object_id(ace_info.tbl_oid).c_str());

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::aclTableCreate(
        _In_ sai_object_id_t object_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    auto sid = sai_serialize_object_id(object_id);

    CHECK_STATUS(create_internal(SAI_OBJECT_TYPE_ACL_TABLE, sid, switch_id, attr_count, attr_list));

    SWSS_LOG_NOTICE("ACL table %s created", sid.c_str());

    return aclDefaultAllowConfigure(object_id);
}

sai_status_t SwitchStateBase::aclTableRemove(
    _In_ const std::string &serializedObjectId)
{
    sai_object_id_t tbl_oid;

    sai_deserialize_object_id(serializedObjectId, tbl_oid);

    CHECK_STATUS(remove_internal(SAI_OBJECT_TYPE_ACL_TABLE, serializedObjectId));

    return AclTblRemove(tbl_oid);
}

sai_status_t SwitchStateBase::aclDefaultAllowConfigure (
    _In_ sai_object_id_t tbl_oid)
{
    sai_attribute_t attr[2];

    attr[0].id = SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE;
    attr[0].value.aclfield.enable = true;
    attr[0].value.aclfield.data.s32 = SAI_ACL_IP_TYPE_IPV4ANY;

    attr[1].id = SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE;
    attr[1].value.aclfield.enable = true;
    attr[1].value.aclfield.data.s32 = SAI_ACL_IP_TYPE_IPV6ANY;

    vpp_acl_t *acl;

    acl = (vpp_acl_t *) calloc(1, sizeof(vpp_acl_t) + (2 * sizeof(vpp_acl_rule_t)));
    if (!acl) {
	return SAI_STATUS_FAILURE;
    }
    acl->count = 2;
    char aclname[64];

    auto sid = sai_serialize_object_id(tbl_oid);

    snprintf(aclname, sizeof(aclname), "sonic_acl_%s", sid.c_str());
    acl->acl_name = aclname;

    vpp_acl_rule_t *rule = &acl->rules[0];

    acl_rule_field_update((sai_acl_entry_attr_t) attr[0].id, &attr[0].value, rule);
    rule->action = VPP_ACL_ACTION_API_PERMIT;

    rule = &acl->rules[1];

    acl_rule_field_update((sai_acl_entry_attr_t) attr[1].id, &attr[1].value, rule);
    rule->action = VPP_ACL_ACTION_API_PERMIT;

    uint32_t acl_swindex = 0;
    sai_status_t status;

    status = vpp_acl_add_replace(acl, &acl_swindex, false);
    if (status == SAI_STATUS_SUCCESS) {
	m_acl_swindex_map[tbl_oid] = acl_swindex;
    }

    free(acl);
    SWSS_LOG_NOTICE("Default Allow all ACL for table %s added, status %d swindex %u",
		    sid.c_str(), status, acl_swindex);
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::AclTblRemove(
    _In_ sai_object_id_t tbl_oid)
{
    sai_status_t status;

    auto it = m_acl_tbl_rules_map.find(tbl_oid);

    if (it != m_acl_tbl_rules_map.end()) {
	std::list<sai_object_id_t>& member_list = it->second;

	if (member_list.size()) {
	    member_list.clear();
	}
	m_acl_tbl_rules_map.erase(it);
    }

    status = tunterm_acl_delete(tbl_oid);

    auto vpp_idx_it = m_acl_swindex_map.find(tbl_oid);
    if (vpp_idx_it == m_acl_swindex_map.end()) {
	SWSS_LOG_WARN("No ACL configured for table %s", sai_serialize_object_id(tbl_oid).c_str());
	return SAI_STATUS_FAILURE;
    }
    uint32_t acl_swindex = vpp_idx_it->second;

    status = vpp_acl_del(acl_swindex);

    if (status == SAI_STATUS_SUCCESS) {
	m_acl_swindex_map.erase(vpp_idx_it);
    }
    SWSS_LOG_NOTICE("ACL table %s remove swindex %u status %d",
		    sai_serialize_object_id(tbl_oid).c_str(), acl_swindex, status);

    return status;
}

sai_status_t SwitchStateBase::addRemoveAclEntrytoMap(
    _In_ sai_object_id_t entry_id,
    _In_ sai_object_id_t tbl_oid,
    _In_ bool is_add)
{
    SWSS_LOG_ENTER();

    auto it = m_acl_tbl_rules_map.find(tbl_oid);

    if (it == m_acl_tbl_rules_map.end()) {

	if (!is_add) {
	    auto sid = sai_serialize_object_id(entry_id);
	    SWSS_LOG_ERROR("ACL entry with id %s not found in tbl %s", sid.c_str(),
			   sai_serialize_object_id(tbl_oid).c_str());
	    return SAI_STATUS_FAILURE;
	}

	std::list<sai_object_id_t> member_list;

	member_list = { entry_id };
	m_acl_tbl_rules_map[tbl_oid] = member_list;

    } else {
	std::list<sai_object_id_t>& member_list = it->second;

	if (!is_add) {
	    member_list.remove(entry_id);
	    return SAI_STATUS_SUCCESS;
	}
	member_list.push_back(entry_id);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::AclAddRemoveCheck(
    _In_ sai_object_id_t tbl_oid)
{
    auto it = m_acl_tbl_rules_map.find(tbl_oid);

    if (it == m_acl_tbl_rules_map.end()) {
	return SAI_STATUS_SUCCESS;
    }
    sai_status_t status = SAI_STATUS_SUCCESS;
    std::list<sai_object_id_t>& member_list = it->second;

    if (member_list.size()) {
	status = AclTblConfig(tbl_oid);
    }
    return status;
}

sai_status_t SwitchStateBase::setAclEntry(
    _In_ sai_object_id_t entry_id,
    _In_ const sai_attribute_t* attr)
{
    SWSS_LOG_ENTER();

    if (attr && attr->id == SAI_ACL_ENTRY_ATTR_ACTION_MACSEC_FLOW)
    {
        return setAclEntryMACsecFlowActive(entry_id, attr);
    }
    auto sid = sai_serialize_object_id(entry_id);

    set_internal(SAI_OBJECT_TYPE_ACL_ENTRY, sid, attr);

    sai_object_id_t tbl_oid;

    if (getAclTableId(entry_id, &tbl_oid) != SAI_STATUS_SUCCESS) {
	return SAI_STATUS_FAILURE;
    }

    auto status = AclAddRemoveCheck(tbl_oid);

    SWSS_LOG_NOTICE("ACL entry %s set in table %s set status %d",
		    sid.c_str(),
		    sai_serialize_object_id(tbl_oid).c_str(),
		    status);
    return status;
}

sai_status_t SwitchStateBase::createAclEntry(
        _In_ sai_object_id_t object_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto sid = sai_serialize_object_id(object_id);

    CHECK_STATUS(create_internal(SAI_OBJECT_TYPE_ACL_ENTRY, sid, switch_id, attr_count, attr_list));

    sai_object_id_t tbl_oid;

    if (getAclTableId(object_id, &tbl_oid) != SAI_STATUS_SUCCESS) {
	return SAI_STATUS_FAILURE;
    }
    sai_status_t status;

    status = addRemoveAclEntrytoMap(object_id, tbl_oid, true);
    if (status == SAI_STATUS_SUCCESS) {
	status = AclAddRemoveCheck(tbl_oid);
    }
    return status;
}

sai_status_t SwitchStateBase::removeAclEntry(
        _In_ const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();

    sai_object_id_t entry_oid;

    sai_deserialize_object_id(serializedObjectId, entry_oid);

    sai_object_id_t tbl_oid;

    if (getAclTableId(entry_oid, &tbl_oid) != SAI_STATUS_SUCCESS) {
	return SAI_STATUS_FAILURE;
    }

    sai_status_t status;

    status = addRemoveAclEntrytoMap(entry_oid, tbl_oid, false);
    if (status == SAI_STATUS_SUCCESS) {
	status = AclAddRemoveCheck(tbl_oid);
    }
    remove_internal(SAI_OBJECT_TYPE_ACL_ENTRY, serializedObjectId);

    return status;
}

sai_status_t SwitchStateBase::getAclTableGroupId(
    _In_ sai_object_id_t member_oid,
    _Out_ sai_object_id_t *tbl_grp_oid)
{
    sai_attribute_t attr;

    attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID;
    if (get(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, member_oid, 1, &attr) != SAI_STATUS_SUCCESS) {
	auto sid = sai_serialize_object_id(member_oid);

	SWSS_LOG_NOTICE("ACL table group oid for acl grp member id %s not found", sid.c_str());
	return SAI_STATUS_FAILURE;
    }

    *tbl_grp_oid = attr.value.oid;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::addRemoveAclGrpMbr(
    _In_ sai_object_id_t member_oid,
    _In_ sai_object_id_t tbl_grp_oid,
    _In_ bool is_add)
{
    SWSS_LOG_ENTER();

    auto it = m_acl_tbl_grp_mbr_map.find(tbl_grp_oid);

    if (it == m_acl_tbl_grp_mbr_map.end()) {

	if (!is_add) {
	    auto sid = sai_serialize_object_id(member_oid);
	    SWSS_LOG_ERROR("ACL group member with id %s not found in tbl %s", sid.c_str(),
			   sai_serialize_object_id(tbl_grp_oid).c_str());
	    return SAI_STATUS_FAILURE;
	}

	std::list<sai_object_id_t> member_list;

	member_list = { member_oid };
	m_acl_tbl_grp_mbr_map[tbl_grp_oid] = member_list;

    } else {
	std::list<sai_object_id_t>& member_list = it->second;

	if (!is_add) {
	    member_list.remove(member_oid);
	} else {
	    member_list.push_back(member_oid);
	}
    }

    sai_attribute_t attr;

    attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID;
    if (get(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, member_oid, 1, &attr) != SAI_STATUS_SUCCESS) {
	auto sid = sai_serialize_object_id(member_oid);

	SWSS_LOG_NOTICE("ACL group member %s table id not found", sid.c_str());
	return SAI_STATUS_SUCCESS;
    }

    aclBindUnbindPorts(tbl_grp_oid, attr.value.oid, is_add);
    
    SWSS_LOG_NOTICE("ACL group member %s %s table group %s",
		    sai_serialize_object_id(member_oid).c_str(),
		    is_add ? "added to" : "removed from",
		    sai_serialize_object_id(tbl_grp_oid).c_str());
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::createAclGrpMbr(
        _In_ sai_object_id_t object_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto sid = sai_serialize_object_id(object_id);

    CHECK_STATUS(create_internal(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER,
				 sid, switch_id, attr_count, attr_list));

    sai_object_id_t tbl_grp_oid;

    if (getAclTableGroupId(object_id, &tbl_grp_oid) != SAI_STATUS_SUCCESS) {
	return SAI_STATUS_FAILURE;
    }
    sai_status_t status;

    status = addRemoveAclGrpMbr(object_id, tbl_grp_oid, true);

    return status;
}

sai_status_t SwitchStateBase::removeAclGrpMbr(
        _In_ const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();

    sai_object_id_t member_oid;

    sai_deserialize_object_id(serializedObjectId, member_oid);

    sai_object_id_t tbl_grp_oid;

    if (getAclTableGroupId(member_oid, &tbl_grp_oid) != SAI_STATUS_SUCCESS) {
	return SAI_STATUS_FAILURE;
    }

    sai_status_t status;

    status = addRemoveAclGrpMbr(member_oid, tbl_grp_oid, false);

    SWSS_LOG_NOTICE("Remove Acl grp member %s status %d",
		    serializedObjectId.c_str(), status);

    remove_internal(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, serializedObjectId);

    return status;
}

sai_status_t SwitchStateBase::setAclGrpMbr(
        _In_ sai_object_id_t member_oid,
        _In_ const sai_attribute_t* attr)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_SUCCESS;

    sai_object_id_t tbl_grp_oid = SAI_NULL_OBJECT_ID;

    getAclTableGroupId(member_oid, &tbl_grp_oid);

    if (attr->id == SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID) {
	if (tbl_grp_oid == SAI_NULL_OBJECT_ID) {
	    status = addRemoveAclGrpMbr(member_oid, tbl_grp_oid, true);
	} else {
	    status = addRemoveAclGrpMbr(member_oid, tbl_grp_oid, false);
	    if (status == SAI_STATUS_SUCCESS) {
		status = addRemoveAclGrpMbr(member_oid, attr->value.oid, true);
	    }
	}
    }
    auto sid = sai_serialize_object_id(member_oid);

    SWSS_LOG_NOTICE("ACL grp member %s set attr %d", sid.c_str(), attr->id);

    set_internal(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, sid, attr);

    return status;
}

sai_status_t SwitchStateBase::removeAclGrp(
    _In_ const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();

    sai_object_id_t tbl_grp_oid;

    sai_deserialize_object_id(serializedObjectId, tbl_grp_oid);

    auto it = m_acl_tbl_grp_mbr_map.find(tbl_grp_oid);

    if (it != m_acl_tbl_grp_mbr_map.end()) {

	std::list<sai_object_id_t>& member_list = it->second;
	sai_status_t status;

	for (sai_object_id_t member_oid: member_list) {

	    status = addRemoveAclGrpMbr(member_oid, tbl_grp_oid, false);
	    if (status != SAI_STATUS_SUCCESS) {
		SWSS_LOG_WARN("Failed to delete ACL tbl grp member %s from group %s",
			      sai_serialize_object_id(member_oid).c_str(),
			      serializedObjectId.c_str());
	    }
	}
	m_acl_tbl_grp_mbr_map.erase(it);
    }
    SWSS_LOG_NOTICE("Remove ACL group %s", serializedObjectId.c_str());

    return remove_internal(SAI_OBJECT_TYPE_ACL_TABLE_GROUP, serializedObjectId);
}

sai_status_t SwitchStateBase::addRemovePortTblGrp(
    _In_ sai_object_id_t port_oid,
    _In_ sai_object_id_t tbl_grp_oid,
    _In_ bool is_add)
{
    auto it = m_acl_tbl_grp_ports_map.find(tbl_grp_oid);

    if (it == m_acl_tbl_grp_ports_map.end()) {
	if (!is_add) {
	    SWSS_LOG_NOTICE("port id %s delete failed, no table group %s",
			    sai_serialize_object_id(port_oid).c_str(),
			    sai_serialize_object_id(tbl_grp_oid).c_str());
	    return SAI_STATUS_FAILURE;
	}
	std::list<sai_object_id_t> ports_list = { port_oid };
	m_acl_tbl_grp_ports_map[tbl_grp_oid] = ports_list;
    } else {
	std::list<sai_object_id_t>& ports_list = it->second;
	if (is_add) {
	    ports_list.push_back(port_oid);
	} else {
	    ports_list.remove(port_oid);
	}
    }
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::aclBindUnbindPort(
        _In_ sai_object_id_t port_oid,
        _In_ sai_object_id_t tbl_grp_oid,
	_In_ bool is_input,
	_In_ bool is_bind)
{
    SWSS_LOG_ENTER();

    addRemovePortTblGrp(port_oid, tbl_grp_oid, is_bind);

    auto it = m_acl_tbl_grp_mbr_map.find(tbl_grp_oid);

    if (it == m_acl_tbl_grp_mbr_map.end()) {
	auto sid = sai_serialize_object_id(tbl_grp_oid);
	SWSS_LOG_NOTICE("ACL tbl group with id %s not found", sid.c_str());
	/* 
	 * The tbl group is not created until a group member is added. The bind port
	 * will be called later when a group member is added.
	 */
	return SAI_STATUS_SUCCESS;
    }

    std::string hwif_name;

    if (!vpp_get_hwif_name(port_oid, 0, hwif_name)) {
	SWSS_LOG_WARN("VPP hwif name not found for port %s", sai_serialize_object_id(port_oid).c_str());
	return SAI_STATUS_FAILURE;
    }
    std::list<sai_object_id_t>& member_list = it->second;

    for (auto member_oid: member_list) {
	sai_attribute_t attr;

	attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID;
	if (get(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, member_oid, 1, &attr) != SAI_STATUS_SUCCESS) {
	    auto sid = sai_serialize_object_id(member_oid);

	    SWSS_LOG_NOTICE("ACL table oid for acl grp member id %s not found", sid.c_str());
	    continue;
	}
	auto tbl_oid = attr.value.oid;
	auto vpp_idx_it = m_acl_swindex_map.find(tbl_oid);
	if (vpp_idx_it == m_acl_swindex_map.end()) {
	    auto sid = sai_serialize_object_id(tbl_oid);
	    SWSS_LOG_NOTICE("VPP swindex for ACL table oid %s not found", sid.c_str());
	    continue;
	}
	auto acl_swindex = vpp_idx_it->second;
	int ret;

	if (is_bind)
	    ret = vpp_acl_interface_bind(hwif_name.c_str(), acl_swindex, is_input);
	else
	    ret = vpp_acl_interface_unbind(hwif_name.c_str(), acl_swindex, is_input);

	if (ret != 0) {
	    auto sid = sai_serialize_object_id(tbl_oid);
	    SWSS_LOG_ERROR("VPP Acl tbl %s (swindex %u) %s failed", sid.c_str(), acl_swindex,
			   is_bind ? "bind": "unbind");
	    return SAI_STATUS_FAILURE;
	}
	SWSS_LOG_NOTICE("ACL table %s %s to port %s", sai_serialize_object_id(tbl_oid).c_str(),
			is_bind ? "bind": "unbind", hwif_name.c_str());
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::aclBindUnbindPorts(
        _In_ sai_object_id_t tbl_grp_oid,
	_In_ sai_object_id_t tbl_oid,
	_In_ bool is_bind)
{
    SWSS_LOG_ENTER();

    auto it = m_acl_tbl_grp_ports_map.find(tbl_grp_oid);

    if (it == m_acl_tbl_grp_ports_map.end()) {
	auto sid = sai_serialize_object_id(tbl_grp_oid);
	SWSS_LOG_NOTICE("ACL tbl group with id %s not found in acl ports map", sid.c_str());
	return SAI_STATUS_SUCCESS;
    }
    sai_attribute_t attr;

    attr.id = SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE;
    if (get(SAI_OBJECT_TYPE_ACL_TABLE_GROUP, tbl_grp_oid, 1, &attr) != SAI_STATUS_SUCCESS) {
	auto sid = sai_serialize_object_id(tbl_grp_oid);

	SWSS_LOG_NOTICE("ACL table group %s direction not found", sid.c_str());
	return SAI_STATUS_SUCCESS;
    }

    int dir = attr.value.s32;
    bool is_input;

    switch (dir) {
    case SAI_ACL_STAGE_INGRESS:
	is_input = true;
	break;

    case SAI_ACL_STAGE_EGRESS:
	is_input = false;
	break;

    default:
    {
	auto sid = sai_serialize_object_id(tbl_grp_oid);
	SWSS_LOG_NOTICE("ACL table group %s direction %d", sid.c_str(), dir);
	return SAI_STATUS_SUCCESS;
    }
    }

    auto vpp_idx_it = m_acl_swindex_map.find(tbl_oid);
    if (vpp_idx_it == m_acl_swindex_map.end()) {
	auto sid = sai_serialize_object_id(tbl_oid);
	SWSS_LOG_NOTICE("VPP swindex for ACL table oid %s not found", sid.c_str());
	return SAI_STATUS_FAILURE;
    }
    auto acl_swindex = vpp_idx_it->second;

    std::list<sai_object_id_t>& member_list = it->second;
    std::list<std::string> hwif_list;
    std::string hwif_name;
    int ret;

    for (auto port_oid: member_list) {

	if (!vpp_get_hwif_name(port_oid, 0, hwif_name)) {
	    SWSS_LOG_WARN("VPP hwif name not found for port %s", sai_serialize_object_id(port_oid).c_str());
	    continue;
	}


	if (is_bind) {
	    ret = vpp_acl_interface_bind(hwif_name.c_str(), acl_swindex, is_input);
        if(ret == 0) {
            hwif_list.push_back(hwif_name);
            tunterm_acl_bindunbind(tbl_oid, true, hwif_name);
        }
    }
	else {
	    ret = vpp_acl_interface_unbind(hwif_name.c_str(), acl_swindex, is_input);
        if(ret == 0) {
            hwif_list.remove(hwif_name);
            tunterm_acl_bindunbind(tbl_oid, false, hwif_name);
        }
    }
	if (ret != 0) {
	    auto sid = sai_serialize_object_id(tbl_oid);
	    SWSS_LOG_ERROR("VPP Acl tbl %s (swindex %u) %s failed status %d",
			   sid.c_str(), acl_swindex,
			   is_bind ? "bind": "unbind", ret);
	    continue;
	}
	SWSS_LOG_NOTICE("ACL table %s %s port %s", sai_serialize_object_id(tbl_oid).c_str(),
			is_bind ? "bound to": "unbound from", hwif_name.c_str());
    }

    for(auto hwif: hwif_list) {
        m_acl_tbl_hw_ports_map[tbl_oid].push_back(hwif);
    }
    

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::getAclEntryStats(
    _In_ sai_object_id_t ace_cntr_oid,
    _In_ uint32_t attr_count,
    _Out_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_FAILURE;
    uint32_t acl_index, ace_index;

    if (aclGetVppIndices(ace_cntr_oid, &acl_index, &ace_index) == SAI_STATUS_SUCCESS) {
	vpp_ace_stats_t ace_stats;

	if (vpp_acl_ace_stats_query(acl_index, ace_index, &ace_stats) == 0) {

	    for (uint32_t i = 0; i < attr_count; i++) {
		if (attr_list[i].id == SAI_ACL_COUNTER_ATTR_PACKETS) {
		    attr_list[i].value.u64 = ace_stats.packets;
		} else if (attr_list[i].id == SAI_ACL_COUNTER_ATTR_BYTES) {
		    attr_list[i].value.u64 = ace_stats.bytes;
		}
	    }
	    status = SAI_STATUS_SUCCESS;
	}
    }

    return status;
}