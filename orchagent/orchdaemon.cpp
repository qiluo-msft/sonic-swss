#include <unistd.h>
#include <unordered_map>
#include "orchdaemon.h"
#include "logger.h"
#include <sairedis.h>
#include "warm_restart.h"

#define SAI_SWITCH_ATTR_CUSTOM_RANGE_BASE SAI_SWITCH_ATTR_CUSTOM_RANGE_START
#include "sairedis.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time */
#define SELECT_TIMEOUT 1000
#define PFC_WD_POLL_MSECS 100

extern sai_switch_api_t*           sai_switch_api;
extern sai_object_id_t             gSwitchId;
extern void syncd_apply_view();
/*
 * Global orch daemon variables
 */
PortsOrch *gPortsOrch;
FdbOrch *gFdbOrch;
IntfsOrch *gIntfsOrch;
NeighOrch *gNeighOrch;
RouteOrch *gRouteOrch;
AclOrch *gAclOrch;
CrmOrch *gCrmOrch;
BufferOrch *gBufferOrch;
SwitchOrch *gSwitchOrch;

OrchDaemon::OrchDaemon(DBConnector *applDb, DBConnector *configDb, DBConnector *stateDb) :
        m_applDb(applDb),
        m_configDb(configDb),
        m_stateDb(stateDb)
{
    SWSS_LOG_ENTER();
}

OrchDaemon::~OrchDaemon()
{
    SWSS_LOG_ENTER();
    for (Orch *o : m_orchList)
        delete(o);
}

bool OrchDaemon::init()
{
    SWSS_LOG_ENTER();

    string platform = getenv("platform") ? getenv("platform") : "";

    gSwitchOrch = new SwitchOrch(m_applDb, APP_SWITCH_TABLE_NAME);

    const int portsorch_base_pri = 40;

    vector<table_name_with_pri_t> ports_tables = {
        { APP_PORT_TABLE_NAME,        portsorch_base_pri + 5 },
        { APP_VLAN_TABLE_NAME,        portsorch_base_pri + 2 },
        { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri     },
        { APP_LAG_TABLE_NAME,         portsorch_base_pri + 4 },
        { APP_LAG_MEMBER_TABLE_NAME,  portsorch_base_pri     }
    };

    gCrmOrch = new CrmOrch(m_configDb, CFG_CRM_TABLE_NAME);
    gPortsOrch = new PortsOrch(m_applDb, ports_tables);
    gFdbOrch = new FdbOrch(m_applDb, APP_FDB_TABLE_NAME, gPortsOrch);
    gIntfsOrch = new IntfsOrch(m_applDb, APP_INTF_TABLE_NAME);
    gNeighOrch = new NeighOrch(m_applDb, APP_NEIGH_TABLE_NAME, gIntfsOrch);
    gRouteOrch = new RouteOrch(m_applDb, APP_ROUTE_TABLE_NAME, gNeighOrch);
    CoppOrch  *copp_orch  = new CoppOrch(m_applDb, APP_COPP_TABLE_NAME);
    TunnelDecapOrch *tunnel_decap_orch = new TunnelDecapOrch(m_applDb, APP_TUNNEL_DECAP_TABLE_NAME);

    vector<string> qos_tables = {
        CFG_TC_TO_QUEUE_MAP_TABLE_NAME,
        CFG_SCHEDULER_TABLE_NAME,
        CFG_DSCP_TO_TC_MAP_TABLE_NAME,
        CFG_QUEUE_TABLE_NAME,
        CFG_PORT_QOS_MAP_TABLE_NAME,
        CFG_WRED_PROFILE_TABLE_NAME,
        CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
        CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
        CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME
    };
    QosOrch *qos_orch = new QosOrch(m_configDb, qos_tables);

    vector<string> buffer_tables = {
        CFG_BUFFER_POOL_TABLE_NAME,
        CFG_BUFFER_PROFILE_TABLE_NAME,
        CFG_BUFFER_QUEUE_TABLE_NAME,
        CFG_BUFFER_PG_TABLE_NAME,
        CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
        CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
    };
    gBufferOrch = new BufferOrch(m_configDb, buffer_tables);

    TableConnector appDbMirrorSession(m_applDb, APP_MIRROR_SESSION_TABLE_NAME);
    TableConnector confDbMirrorSession(m_configDb, CFG_MIRROR_SESSION_TABLE_NAME);
    MirrorOrch *mirror_orch = new MirrorOrch(appDbMirrorSession, confDbMirrorSession, gPortsOrch, gRouteOrch, gNeighOrch, gFdbOrch);
    VRFOrch *vrf_orch = new VRFOrch(m_configDb, CFG_VRF_TABLE_NAME);

    TableConnector confDbAclTable(m_configDb, CFG_ACL_TABLE_NAME);
    TableConnector confDbAclRuleTable(m_configDb, CFG_ACL_RULE_TABLE_NAME);
    TableConnector stateDbLagTable(m_stateDb, STATE_LAG_TABLE_NAME);

    vector<TableConnector> acl_table_connectors = {
        confDbAclTable,
        confDbAclRuleTable,
        stateDbLagTable
    };

    vector<string> dtel_tables = {
        CFG_DTEL_TABLE_NAME,
        CFG_DTEL_REPORT_SESSION_TABLE_NAME,
        CFG_DTEL_INT_SESSION_TABLE_NAME,
        CFG_DTEL_QUEUE_REPORT_TABLE_NAME,
        CFG_DTEL_EVENT_TABLE_NAME
    };

    /*
     * The order of the orch list is important for state restore of warm start and
     * the queued processing in m_toSync map after gPortsOrch->isInitDone() is set.
     *
     * For the multiple consumers in ports_tables, tasks for LAG_TABLE is processed before VLAN_TABLE
     * when iterating ConsumerMap.
     * That is ensured implicitly by the order of map key, "LAG_TABLE" is smaller than "VLAN_TABLE" in lexicographic order.
     */
    m_orchList = { gSwitchOrch, gCrmOrch, gBufferOrch, gPortsOrch, gIntfsOrch, gNeighOrch, gRouteOrch, copp_orch, tunnel_decap_orch, qos_orch};

    bool initialize_dtel = false;
    if (platform == BFN_PLATFORM_SUBSTRING || platform == VS_PLATFORM_SUBSTRING)
    {
        sai_attr_capability_t capability;
        capability.create_implemented = true;

    /* Will uncomment this when saiobject.h support is added to SONiC */
    /*
    sai_status_t status;

        status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_DTEL, SAI_DTEL_ATTR_SWITCH_ID, &capability);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Could not query Dataplane telemetry capability %d", status);
            exit(EXIT_FAILURE);
        }
    */

        if (capability.create_implemented)
        {
            initialize_dtel = true;
        }
    }

    DTelOrch *dtel_orch = NULL;
    if (initialize_dtel)
    {
        dtel_orch = new DTelOrch(m_configDb, dtel_tables, gPortsOrch);
        m_orchList.push_back(dtel_orch);
        gAclOrch = new AclOrch(acl_table_connectors, gPortsOrch, mirror_orch, gNeighOrch, gRouteOrch, dtel_orch);
    } else {
        gAclOrch = new AclOrch(acl_table_connectors, gPortsOrch, mirror_orch, gNeighOrch, gRouteOrch);
    }

    m_orchList.push_back(gFdbOrch);
    m_orchList.push_back(mirror_orch);
    m_orchList.push_back(gAclOrch);
    m_orchList.push_back(vrf_orch);

    m_select = new Select();

    vector<string> flex_counter_tables = {
        CFG_FLEX_COUNTER_TABLE_NAME
    };

    m_orchList.push_back(new FlexCounterOrch(m_configDb, flex_counter_tables));

    vector<string> pfc_wd_tables = {
        CFG_PFC_WD_TABLE_NAME
    };

    if (platform == MLNX_PLATFORM_SUBSTRING)
    {

        static const vector<sai_port_stat_t> portStatIds =
        {
            SAI_PORT_STAT_PFC_0_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_1_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_2_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_3_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_5_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_6_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_7_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_0_RX_PKTS,
            SAI_PORT_STAT_PFC_1_RX_PKTS,
            SAI_PORT_STAT_PFC_2_RX_PKTS,
            SAI_PORT_STAT_PFC_3_RX_PKTS,
            SAI_PORT_STAT_PFC_4_RX_PKTS,
            SAI_PORT_STAT_PFC_5_RX_PKTS,
            SAI_PORT_STAT_PFC_6_RX_PKTS,
            SAI_PORT_STAT_PFC_7_RX_PKTS,
        };

        static const vector<sai_queue_stat_t> queueStatIds =
        {
            SAI_QUEUE_STAT_PACKETS,
            SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,
        };

        static const vector<sai_queue_attr_t> queueAttrIds;

        m_orchList.push_back(new PfcWdSwOrch<PfcWdZeroBufferHandler, PfcWdLossyHandler>(
                    m_configDb,
                    pfc_wd_tables,
                    portStatIds,
                    queueStatIds,
                    queueAttrIds,
                    PFC_WD_POLL_MSECS));
    }
    else if (platform == BRCM_PLATFORM_SUBSTRING)
    {
        static const vector<sai_port_stat_t> portStatIds =
        {
            SAI_PORT_STAT_PFC_0_RX_PKTS,
            SAI_PORT_STAT_PFC_1_RX_PKTS,
            SAI_PORT_STAT_PFC_2_RX_PKTS,
            SAI_PORT_STAT_PFC_3_RX_PKTS,
            SAI_PORT_STAT_PFC_4_RX_PKTS,
            SAI_PORT_STAT_PFC_5_RX_PKTS,
            SAI_PORT_STAT_PFC_6_RX_PKTS,
            SAI_PORT_STAT_PFC_7_RX_PKTS,
            SAI_PORT_STAT_PFC_0_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_1_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_2_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_5_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_6_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_7_ON2OFF_RX_PKTS,
        };

        static const vector<sai_queue_stat_t> queueStatIds =
        {
            SAI_QUEUE_STAT_PACKETS,
            SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,
        };

        static const vector<sai_queue_attr_t> queueAttrIds =
        {
            SAI_QUEUE_ATTR_PAUSE_STATUS,
        };

        m_orchList.push_back(new PfcWdSwOrch<PfcWdAclHandler, PfcWdLossyHandler>(
                    m_configDb,
                    pfc_wd_tables,
                    portStatIds,
                    queueStatIds,
                    queueAttrIds,
                    PFC_WD_POLL_MSECS));
    }

    m_orchList.push_back(&CounterCheckOrch::getInstance(m_configDb));

    return true;
}

/* Flush redis through sairedis interface */
void OrchDaemon::flush()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_REDIS_SWITCH_ATTR_FLUSH;
    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to flush redis pipeline %d", status);
        exit(EXIT_FAILURE);
    }
}

void OrchDaemon::start()
{
    SWSS_LOG_ENTER();

    warmRestoreAndSyncUp();

    for (Orch *o : m_orchList)
    {
        m_select->addSelectables(o->getSelectables());
    }

    while (true)
    {
        Selectable *s;
        int ret;

        ret = m_select->select(&s, SELECT_TIMEOUT);

        if (ret == Select::ERROR)
        {
            SWSS_LOG_NOTICE("Error: %s!\n", strerror(errno));
            continue;
        }

        if (ret == Select::TIMEOUT)
        {
            continue;
        }

        auto *c = (Executor *)s;
        c->execute();

        /* After each iteration, periodically check all m_toSync map to
         * execute all the remaining tasks that need to be retried. */

        /* TODO: Abstract Orch class to have a specific todo list */
        for (Orch *o : m_orchList)
            o->doTask();

        /* Let sairedis to flush all SAI function call to ASIC DB.
         * Normally the redis pipeline will flush when enough request
         * accumulated. Still it is possible that small amount of
         * requests live in it. When the daemon has finished events/tasks, it
         * is a good chance to flush the pipeline before next select happened.
         */
        flush();
    }
}

/*
 * Try to perform orchagent state restore and dynamic states sync up if
 * warm start reqeust is detected.
 */
void OrchDaemon::warmRestoreAndSyncUp()
{
    if (!WarmStart::isWarmStart())
    {
        return;
    }

    WarmStart::setWarmStartState("orchagent", WarmStart::INIT);

    for (Orch *o : m_orchList)
    {
        o->bake();
    }

    /*
     * First iteration is to handle all the existing data in predefined order.
     */
    for (Orch *o : m_orchList)
    {
        o->doTask();
    }
    /*
     * Drain remaining data that are out of order like LAG_MEMBER_TABLE and VLAN_MEMBER_TABLE
     * since they were checked before LAG_TABLE and VLAN_TABLE.
     */
    for (Orch *o : m_orchList)
    {
        o->doTask();
    }

    /*
     * At this point, all the pre-existing data should have been processed properly, and
     * orchagent should be in exact same state of pre-shutdown.
     * Perform restore validation as needed.
     */
    warmRestoreValidation();

    SWSS_LOG_NOTICE("Orchagent state restore done");
    syncd_apply_view();

    /* TODO: perform port and fdb state sync up*/

    /*
     * Note. Arp sync up is handled in neighsyncd.
     * The "RECONCILED" state of orchagent doesn't mean the state related to neighbor is up to date.
     */
    WarmStart::setWarmStartState("orchagent", WarmStart::RECONCILED);
}

/* Perform basic validation after start restore for warm start */
bool OrchDaemon::warmRestoreValidation()
{
    /*
     * No pending task should exist for any of the consumer at this point.
     * All the prexisting data in appDB and configDb have been read and processed.
     */
    // TODO: Update this section accordingly once pre-warmStart consistency validation is ready.
    bool first = true;
    for (Orch *o : m_orchList)
    {
        /*
         * Get tasks to sync for consumers of each orch being managed by this orch daemon
         */
        for (const auto& c : o->rangeConsumer())
        {
            if (!empty(c->rangeToSync()) && first)
            {
                SWSS_LOG_NOTICE("There are pending consumer tasks after restore: ");
                first = false;
            }

            for (const KeyOpFieldsValuesTuple& tuple : c->rangeToSync())
            {
                SWSS_LOG_NOTICE("%s", c->format(tuple).c_str());
            }
        }
    }

    WarmStart::setWarmStartState("orchagent", WarmStart::RESTORED);
    return true;
}
