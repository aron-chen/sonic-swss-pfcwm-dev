/* MODULE NAME: watermarkorch_ut.cpp
* PURPOSE: 
*	{1. Unit test file for the watermarkorch.cpp. (google test)
*
* NOTES:
*	{1. Mock the PortsOrch::isPortReady() to always return true.
*
* HISTORY:
*	 05/06/2019  -- Ron, Create
*
* Copyright(C)      Accton Corporation, 2019
*/

#include "gtest/gtest.h"
#include "select.h"
#include "notificationproducer.h"
#include "saiattributelist.h"
/* The private variables of the watermarkorch is need in this unit test;
 * change the private to public for watermarkorch.h.
 */
#define private public
#define protected public
#include "watermarkorch.h"
#undef private
#undef protected

#define DEFAULT_TELEMETRY_INTERVAL 120
#define NEW_TELEMETRY_INTERVAL 150
#define FAKE_UNICAST_QUEUE_OID "oid:0x15000000000230"
#define FAKE_MULTICAST_QUEUE_OID "oid:0x15000000010230"
#define CLEAR_PG_HEADROOM_REQUEST "PG_HEADROOM"
#define CLEAR_PG_SHARED_REQUEST "PG_SHARED"
#define CLEAR_QUEUE_SHARED_UNI_REQUEST "Q_SHARED_UNI"
#define CLEAR_QUEUE_SHARED_MULTI_REQUEST "Q_SHARED_MULTI"
#define DEFAULT_BATCH_SIZE 128

/* Global variables */
sai_object_id_t gVirtualRouterId;
sai_object_id_t gUnderlayIfId;
sai_object_id_t gSwitchId = SAI_NULL_OBJECT_ID;
MacAddress gMacAddress;
MacAddress gVxlanMacAddress;
int gBatchSize = DEFAULT_BATCH_SIZE;
bool gSairedisRecord = true;
bool gSwssRecord = true;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;

void syncd_apply_view()
{
}


namespace nsWatermarkrchTest {

using namespace std;

class WatermarkTest : public ::testing::Test
{
    public:
    WatermarkTest()
    {
    }
    ~WatermarkTest()
    {
    }
};

size_t WM_ConsumerAddToSync(Consumer* consumer, std::deque<KeyOpFieldsValuesTuple>& entries)
{
    /* Nothing popped */
    if (entries.empty()) 
    {
        return 0;
    }

    for (auto& entry : entries) 
    {
        string key = kfvKey(entry);
        string op = kfvOp(entry);

        /* If a new task comes or if a DEL task comes, we directly put it into getConsumerTable().m_toSync map */
        if (consumer->m_toSync.find(key) == consumer->m_toSync.end() || op == DEL_COMMAND) 
        {
            consumer->m_toSync[key] = entry;
        }
        /* If an old task is still there, we combine the old task with new task */
        else 
        {
            KeyOpFieldsValuesTuple existing_data = consumer->m_toSync[key];

            auto new_values = kfvFieldsValues(entry);
            auto existing_values = kfvFieldsValues(existing_data);

            for (auto it : new_values) 
            {
                string field = fvField(it);
                string value = fvValue(it);

                auto iu = existing_values.begin();
                while (iu != existing_values.end()) 
                {
                    string ofield = fvField(*iu);
                    if (field == ofield)
                        iu = existing_values.erase(iu);
                    else
                        iu++;
                }
                existing_values.push_back(FieldValueTuple(field, value));
            }
            consumer->m_toSync[key] = KeyOpFieldsValuesTuple(key, op, existing_values);
        }
    }
    return entries.size();
}

static void WM_ClearDB(int db_id)
{
    auto db = swss::DBConnector(db_id, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    RedisReply r(&db, "FLUSHALL", REDIS_REPLY_STATUS);
    r.checkStatusOK();

    return;
}

/* The counter DB is null, so we create a fake table to verify the clear logic */
void WM_CreateFakeWmTable(const string wm_table, WatermarkOrch* wm_orch)
{
    auto qtype = std::vector<swss::FieldValueTuple>({
        { FAKE_UNICAST_QUEUE_OID, "SAI_QUEUE_TYPE_UNICAST" },
        { FAKE_MULTICAST_QUEUE_OID, "SAI_QUEUE_TYPE_MULTICAST" } });

    auto value = std::vector<swss::FieldValueTuple>({
        { "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES", "5" },
        { "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES", "5" },
        { "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES", "5" } });

    if (wm_table == PERSISTENT_WATERMARKS_TABLE)
    {
        wm_orch->m_persistentWatermarkTable->set(COUNTERS_QUEUE_TYPE_MAP, qtype);
        wm_orch->m_persistentWatermarkTable->set(FAKE_UNICAST_QUEUE_OID, value);
        wm_orch->m_persistentWatermarkTable->set(FAKE_MULTICAST_QUEUE_OID, value);
    }
    else if (wm_table == USER_WATERMARKS_TABLE)
    {
        wm_orch->m_userWatermarkTable->set(COUNTERS_QUEUE_TYPE_MAP, qtype);
        wm_orch->m_userWatermarkTable->set(FAKE_UNICAST_QUEUE_OID, value);
        wm_orch->m_userWatermarkTable->set(FAKE_MULTICAST_QUEUE_OID, value);
    }
    else /* wm_table == PERIODIC_WATERMARKS_TABLE */
    {
        wm_orch->m_periodicWatermarkTable->set(COUNTERS_QUEUE_TYPE_MAP, qtype);
        wm_orch->m_periodicWatermarkTable->set(FAKE_UNICAST_QUEUE_OID, value);
        wm_orch->m_periodicWatermarkTable->set(FAKE_MULTICAST_QUEUE_OID, value);
    }

    /* init pg/queue ids */
    for (auto fv: qtype)
    {
        sai_object_id_t id;
        sai_deserialize_object_id(fv.first, id);

        wm_orch->m_pg_ids.push_back(id);

        if (fv.second == "SAI_QUEUE_TYPE_UNICAST")
        {
            wm_orch->m_unicast_queue_ids.push_back(id);
        }
        else
        {
            wm_orch->m_multicast_queue_ids.push_back(id);
        }
    }

    return;
}

bool WM_ValidateClearResult(const string wm_table, const string clr_req)
{
    std::vector<FieldValueTuple> fields;
    auto counters_db = swss::DBConnector(COUNTERS_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    Table tgt_table(&counters_db, wm_table);

    /* It's a fake watermarks table, we put the others fields in unicast table */
    if (clr_req == CLEAR_QUEUE_SHARED_MULTI_REQUEST)
    {
        tgt_table.get(FAKE_MULTICAST_QUEUE_OID, fields);
    }
    else
    {
        tgt_table.get(FAKE_UNICAST_QUEUE_OID, fields);
    }

    for (auto fv: fields)
    {
        if (clr_req == CLEAR_PG_HEADROOM_REQUEST)
        {
            if (fv.first == "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES")
            {
                if (fv.second == "0")
                {
                    return true;
                }
                else
                {
                    return false;
                }
            }
        }
        else if (clr_req == CLEAR_PG_SHARED_REQUEST)
        {
            if (fv.first == "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES")
            {
                if (fv.second == "0")
                {
                    return true;
                }
                else
                {
                    return false;
                }
            }
        }
        else if (clr_req == CLEAR_QUEUE_SHARED_UNI_REQUEST)
        {
            if (fv.first == "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES")
            {
                if (fv.second == "0")
                {
                    return true;
                }
                else
                {
                    return false;
                }
            }
        }
        else if (clr_req == CLEAR_QUEUE_SHARED_MULTI_REQUEST)
        {
            if (fv.first == "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES")
            {
                if (fv.second == "0")
                {
                    return true;
                }
                else
                {
                    return false;
                }
            }
        }
    }
    return false;
}

void WM_SendClearRequest(const string op_type, const string clr_req, WatermarkOrch* wm_orch)
{
    auto appl_db = swss::DBConnector(APPL_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    Select s;
    s.addSelectable(wm_orch->m_clearNotificationConsumer);
    Selectable *sel;

    NotificationProducer np(&appl_db, "WATERMARK_CLEAR_REQUEST");
    vector<FieldValueTuple> values;
    FieldValueTuple tuple("", "");
    values.push_back(tuple);

    np.send(op_type, clr_req, values);

    int result = s.select(&sel, 2000);

    if (result == Select::OBJECT)
    {
        wm_orch->doTask(*wm_orch->m_clearNotificationConsumer);
    }

    return;
}

void WM_FakeConsumer(const string op, const string key, WatermarkOrch* wm_orch)
{
    auto config_db = swss::DBConnector(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    auto consumer = std::unique_ptr<Consumer>(new Consumer(new swss::ConsumerStateTable
        (&config_db, std::string(CFG_WATERMARK_TABLE_NAME), 1, 1), wm_orch, std::string(CFG_WATERMARK_TABLE_NAME)));
    std::vector<swss::FieldValueTuple> values;
    std::deque<KeyOpFieldsValuesTuple> set_data;

    /* support key value */
    if (key == "sprt_key")
    {
        values = std::vector<swss::FieldValueTuple>({ { "interval", "150" } });
    }
    /* unsupport key value */
    else /* (key == "unsprt_key") */
    {
        values = std::vector<swss::FieldValueTuple>({ { "test_key", "test" } });
    }

    KeyOpFieldsValuesTuple rule_attr("TELEMETRY_INTERVAL", op, values );
    set_data = { rule_attr };
    WM_ConsumerAddToSync(consumer.get(), set_data);
    wm_orch->doTask(*consumer);

    return;
}

/*
 * void WatermarkOrch::doTask(Consumer &consumer)
 */
TEST_F(WatermarkTest, TelemetryIntervalConfigTest)
{
    auto config_db = swss::DBConnector(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    WatermarkOrch *wm_orch = new WatermarkOrch(&config_db, CFG_WATERMARK_TABLE_NAME);
    ASSERT_TRUE(wm_orch != (WatermarkOrch *)NULL);

    WM_ClearDB(CONFIG_DB);

    /* Fault-Tolerance Test - Begin*/
    /* test with unsupported key */
    WM_FakeConsumer("SET", "unsprt_key", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == DEFAULT_TELEMETRY_INTERVAL);

    /* test with unsupported operation type */
    WM_FakeConsumer("DEL", "sprt_key", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == DEFAULT_TELEMETRY_INTERVAL);

    /* test with unsupported operation type */
    WM_FakeConsumer("TEST", "sprt_key", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == DEFAULT_TELEMETRY_INTERVAL);
    /* Fault-Tolerance Test - End*/

    /* test with correct key, op, field, value */
    WM_FakeConsumer("SET", "sprt_key", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == NEW_TELEMETRY_INTERVAL);

    WM_ClearDB(CONFIG_DB);
    delete wm_orch;
}

/*
 * void WatermarkOrch::doTask(NotificationConsumer &consumer)
 */
TEST_F(WatermarkTest, WatermarkClearRequestTest)
{
    auto config_db = swss::DBConnector(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    WatermarkOrch *wm_orch = new WatermarkOrch(&config_db, CFG_WATERMARK_TABLE_NAME);
    ASSERT_TRUE(wm_orch != (WatermarkOrch *)NULL);
    bool ret_val = true;

    WM_ClearDB(CONFIG_DB);
    WM_ClearDB(COUNTERS_DB);
    WM_ClearDB(APPL_DB);

    /* Fault-Tolerance Test - Begin*/
    WM_CreateFakeWmTable(PERIODIC_WATERMARKS_TABLE, wm_orch);
    /* Unknown watermark clear request op; The PERIODIC_WATERMARKS only cleared on telemetry period */
    WM_SendClearRequest("PERIODIC", CLEAR_QUEUE_SHARED_UNI_REQUEST, wm_orch);
    ret_val = WM_ValidateClearResult(USER_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == false);

    /* Unknown watermark clear request data */
    WM_SendClearRequest("PERIODIC", "TestData", wm_orch);
    ret_val = WM_ValidateClearResult(PERIODIC_WATERMARKS_TABLE, "TestData");
    EXPECT_TRUE(ret_val == false);
    WM_ClearDB(COUNTERS_DB);
    /* Fault-Tolerance Test - End*/

    /* User Watermarks Clear Test */
    WM_CreateFakeWmTable(USER_WATERMARKS_TABLE, wm_orch);

    WM_SendClearRequest("USER", CLEAR_PG_HEADROOM_REQUEST, wm_orch);
    ret_val = WM_ValidateClearResult(USER_WATERMARKS_TABLE, CLEAR_PG_HEADROOM_REQUEST);
    EXPECT_TRUE(ret_val == true);

    WM_SendClearRequest("USER", CLEAR_PG_SHARED_REQUEST, wm_orch);
    ret_val = WM_ValidateClearResult(USER_WATERMARKS_TABLE, CLEAR_PG_SHARED_REQUEST);
    EXPECT_TRUE(ret_val == true);

    WM_SendClearRequest("USER", CLEAR_QUEUE_SHARED_UNI_REQUEST, wm_orch);
    ret_val = WM_ValidateClearResult(USER_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    WM_SendClearRequest("USER", CLEAR_QUEUE_SHARED_MULTI_REQUEST, wm_orch);
    ret_val = WM_ValidateClearResult(USER_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_MULTI_REQUEST);
    EXPECT_TRUE(ret_val == true);
    WM_ClearDB(COUNTERS_DB);

    /* PERSISTENT Watermarks Clear Test */
    WM_CreateFakeWmTable(PERSISTENT_WATERMARKS_TABLE, wm_orch);

    WM_SendClearRequest("PERSISTENT", CLEAR_PG_HEADROOM_REQUEST, wm_orch);
    ret_val = WM_ValidateClearResult(PERSISTENT_WATERMARKS_TABLE, CLEAR_PG_HEADROOM_REQUEST);
    EXPECT_TRUE(ret_val == true);

    WM_SendClearRequest("PERSISTENT", CLEAR_PG_SHARED_REQUEST, wm_orch);
    ret_val = WM_ValidateClearResult(PERSISTENT_WATERMARKS_TABLE, CLEAR_PG_SHARED_REQUEST);
    EXPECT_TRUE(ret_val == true);

    WM_SendClearRequest("PERSISTENT", CLEAR_QUEUE_SHARED_UNI_REQUEST, wm_orch);
    ret_val = WM_ValidateClearResult(PERSISTENT_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    WM_SendClearRequest("PERSISTENT", CLEAR_QUEUE_SHARED_MULTI_REQUEST, wm_orch);
    ret_val = WM_ValidateClearResult(PERSISTENT_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_MULTI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    WM_ClearDB(APPL_DB);
    WM_ClearDB(COUNTERS_DB);
    WM_ClearDB(CONFIG_DB);

    delete wm_orch;
}

/*
 * void WatermarkOrch::doTask(SelectableTimer &timer)
 */
TEST_F(WatermarkTest, TimerExpiresBehaviorTest)
{
    auto config_db = swss::DBConnector(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    WatermarkOrch *wm_orch = new WatermarkOrch(&config_db, CFG_WATERMARK_TABLE_NAME);
    ASSERT_TRUE(wm_orch != (WatermarkOrch *)NULL);
    bool ret_val = false;

    WM_ClearDB(COUNTERS_DB);
    WM_ClearDB(CONFIG_DB);

    WM_CreateFakeWmTable(PERIODIC_WATERMARKS_TABLE, wm_orch);

    /* The new interval will be assigned to the timer during the timer handling, 
     * and then the orch will reset the interval only when the current timer expires.
     */ 
    WM_FakeConsumer("SET", "sprt_key", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == NEW_TELEMETRY_INTERVAL);
    int cur_interval = 0;

    /* Check the interval before the current timer expires */
    SelectableTimer *faketimer = new SelectableTimer(timespec { .tv_sec = 10, .tv_nsec = 0 });
    wm_orch->doTask(*faketimer);
    cur_interval = wm_orch->m_telemetryTimer->m_interval.it_interval.tv_sec;
    EXPECT_TRUE(cur_interval == DEFAULT_TELEMETRY_INTERVAL);

    /* The periodic watermarks should not be cleared before the timer expires */
    ret_val = WM_ValidateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_PG_HEADROOM_REQUEST);
    EXPECT_TRUE(ret_val == false);

    ret_val = WM_ValidateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_PG_SHARED_REQUEST);
    EXPECT_TRUE(ret_val == false);

    ret_val = WM_ValidateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == false);

    ret_val = WM_ValidateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_MULTI_REQUEST);
    EXPECT_TRUE(ret_val == false);


    /* Check the interval when the current timer expires */
    wm_orch->doTask(*wm_orch->m_telemetryTimer);
    cur_interval = wm_orch->m_telemetryTimer->m_interval.it_interval.tv_sec;
    EXPECT_TRUE(cur_interval == NEW_TELEMETRY_INTERVAL);

    /* The periodic watermarks should be cleared when the current timer expires */
    ret_val = WM_ValidateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_PG_HEADROOM_REQUEST);
    EXPECT_TRUE(ret_val == true);

    ret_val = WM_ValidateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_PG_SHARED_REQUEST);
    EXPECT_TRUE(ret_val == true);

    ret_val = WM_ValidateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    ret_val = WM_ValidateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_MULTI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    WM_ClearDB(COUNTERS_DB);
    WM_ClearDB(CONFIG_DB);

    delete faketimer;
    delete wm_orch;
}
}
