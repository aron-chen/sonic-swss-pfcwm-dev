#include "gtest/gtest.h"

#define private public
#define protected public
    #include "watermarkorch.h"
#undef private
#undef protected

#include "hiredis.h"
#include "orchdaemon.h"
#include "sai_vs.h"
#include "saihelper.h"
#include "notificationproducer.h"
#include "saiattributelist.h"

void syncd_apply_view()
{
}

using namespace std;

/* Global variables */
sai_object_id_t gVirtualRouterId;
sai_object_id_t gUnderlayIfId;
sai_object_id_t gSwitchId = SAI_NULL_OBJECT_ID;
MacAddress gMacAddress;
MacAddress gVxlanMacAddress;

#define DEFAULT_BATCH_SIZE  128
int gBatchSize = DEFAULT_BATCH_SIZE;

bool gSairedisRecord = true;
bool gSwssRecord = true;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;

#define DEFAULT_TELEMETRY_INTERVAL 120
#define NEW_TELEMETRY_INTERVAL 150
#define FAKE_UNICAST_QUEUE_OID "oid:0x15000000000230"
#define FAKE_MULTICAST_QUEUE_OID "oid:0x15000000010230"
#define CLEAR_PG_HEADROOM_REQUEST "PG_HEADROOM"
#define CLEAR_PG_SHARED_REQUEST "PG_SHARED"
#define CLEAR_QUEUE_SHARED_UNI_REQUEST "Q_SHARED_UNI"
#define CLEAR_QUEUE_SHARED_MULTI_REQUEST "Q_SHARED_MULTI"

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

size_t consumerAddToSync(Consumer* consumer, std::deque<KeyOpFieldsValuesTuple>& entries)
{
    /* Nothing popped */
    if (entries.empty()) {
        return 0;
    }

    for (auto& entry : entries) {
        string key = kfvKey(entry);
        string op = kfvOp(entry);

        /* If a new task comes or if a DEL task comes, we directly put it into getConsumerTable().m_toSync map */
        if (consumer->m_toSync.find(key) == consumer->m_toSync.end() || op == DEL_COMMAND) {
            consumer->m_toSync[key] = entry;
        }
        /* If an old task is still there, we combine the old task with new task */
        else {
            KeyOpFieldsValuesTuple existing_data = consumer->m_toSync[key];

            auto new_values = kfvFieldsValues(entry);
            auto existing_values = kfvFieldsValues(existing_data);

            for (auto it : new_values) {
                string field = fvField(it);
                string value = fvValue(it);

                auto iu = existing_values.begin();
                while (iu != existing_values.end()) {
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

static void clearDB(int dbId)
{
    auto db = swss::DBConnector(dbId, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    RedisReply r(&db, "FLUSHALL", REDIS_REPLY_STATUS);
    r.checkStatusOK();
}

// The counter DB is null, so we create a fake table to verify the clear logic
void createFakeWmTable(const string wm_table, WatermarkOrch* wm_orch)
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
    else // PERIODIC
    {
        wm_orch->m_periodicWatermarkTable->set(COUNTERS_QUEUE_TYPE_MAP, qtype);
        wm_orch->m_periodicWatermarkTable->set(FAKE_UNICAST_QUEUE_OID, value);
        wm_orch->m_periodicWatermarkTable->set(FAKE_MULTICAST_QUEUE_OID, value);
    }

    // init pg/queue ids
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

bool validateClearResult(const string wm_table, const string clr_req)
{
    std::vector<FieldValueTuple> fields;
    auto counters_db = swss::DBConnector(COUNTERS_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    Table wmTable(&counters_db, wm_table);

    // It's fake watermarks table, we put the others fields in unicast table
    if (clr_req == CLEAR_QUEUE_SHARED_MULTI_REQUEST)
    {
        wmTable.get(FAKE_MULTICAST_QUEUE_OID, fields);
    }
    else
    {
        wmTable.get(FAKE_UNICAST_QUEUE_OID, fields);
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

void sendClearRequest(const string op_type, const string clr_req, WatermarkOrch* wm_orch)
{
    auto appl_db = swss::DBConnector(APPL_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    Select s;
    s.addSelectable(wm_orch->m_clearNotificationConsumer);
    Selectable *sel;

    NotificationProducer np(&appl_db, "WATERMARK_CLEAR_REQUEST");
    vector<FieldValueTuple> values;
    FieldValueTuple tuple("", "");
    values.push_back(tuple);

    //cout << "Starting sending notification producer" << endl;
    np.send(op_type, clr_req, values);

    int result = s.select(&sel, 2000);

    if (result == Select::OBJECT)
    {
        //cout << "Got notification from producer" << endl;
        wm_orch->doTask(*wm_orch->m_clearNotificationConsumer);
    }
}

void fakeConsumer(const string op, const string key, WatermarkOrch* wm_orch)
{
    auto config_db = swss::DBConnector(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    auto consumer = std::unique_ptr<Consumer>(new Consumer(new swss::ConsumerStateTable(&config_db, std::string(CFG_WATERMARK_TABLE_NAME), 1, 1), wm_orch, std::string(CFG_WATERMARK_TABLE_NAME)));
    std::vector<swss::FieldValueTuple> values;
    std::deque<KeyOpFieldsValuesTuple> setData;

    // support key value
    if (key == "sprtKey")
    {
        values = std::vector<swss::FieldValueTuple>({ { "interval", "150" } });
    }
    // unsupport key value
    else //(key == "unsprtKey")
    {
        values = std::vector<swss::FieldValueTuple>({ { "test_key", "test" } });
    }

    KeyOpFieldsValuesTuple ruleAttr("TELEMETRY_INTERVAL", op, values );
    setData = { ruleAttr };
    consumerAddToSync(consumer.get(), setData);
    wm_orch->doTask(*consumer);
}

/*
 * void WatermarkOrch::doTask(Consumer &consumer)
 */
TEST_F(WatermarkTest, TelemetryIntervalConfig)
{
    auto config_db = swss::DBConnector(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    WatermarkOrch *wm_orch = new WatermarkOrch(&config_db, CFG_WATERMARK_TABLE_NAME);
    ASSERT_TRUE(wm_orch != (WatermarkOrch *)NULL);

    clearDB(CONFIG_DB);

    /* Fault-Tolerance Test - Begin*/
    // test with unsupported key
    fakeConsumer("SET", "unsprtKey", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == DEFAULT_TELEMETRY_INTERVAL);

    // test with unsupported operation type
    fakeConsumer("DEL", "sprtKey", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == DEFAULT_TELEMETRY_INTERVAL);

    // test with unsupported operation type
    fakeConsumer("TEST", "sprtKey", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == DEFAULT_TELEMETRY_INTERVAL);
    /* Fault-Tolerance Test - End*/

    // test with correct key, op, field, value
    fakeConsumer("SET", "sprtKey", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == NEW_TELEMETRY_INTERVAL);

    clearDB(CONFIG_DB);
    delete wm_orch;
}

/*
 * void WatermarkOrch::doTask(NotificationConsumer &consumer)
 */
TEST_F(WatermarkTest, WatermarkClear)
{
    auto config_db = swss::DBConnector(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    WatermarkOrch *wm_orch = new WatermarkOrch(&config_db, CFG_WATERMARK_TABLE_NAME);
    ASSERT_TRUE(wm_orch != (WatermarkOrch *)NULL);
    bool ret_val = true;

    clearDB(CONFIG_DB);
    clearDB(COUNTERS_DB);
    clearDB(APPL_DB);

    /* Fault-Tolerance Test - Begin*/
    createFakeWmTable(PERIODIC_WATERMARKS_TABLE, wm_orch);
    // Unknown watermark clear request op; The PERIODIC_WATERMARKS only cleared on telemetry period
    sendClearRequest("PERIODIC", CLEAR_QUEUE_SHARED_UNI_REQUEST, wm_orch);
    ret_val = validateClearResult(USER_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == false);

    // Unknown watermark clear request data
    sendClearRequest("PERIODIC", "TestData", wm_orch);
    ret_val = validateClearResult(PERIODIC_WATERMARKS_TABLE, "TestData");
    EXPECT_TRUE(ret_val == false);
    clearDB(COUNTERS_DB);
    /* Fault-Tolerance Test - End*/

    // User Watermarks Clear Test
    createFakeWmTable(USER_WATERMARKS_TABLE, wm_orch);

    sendClearRequest("USER", CLEAR_PG_HEADROOM_REQUEST, wm_orch);
    ret_val = validateClearResult(USER_WATERMARKS_TABLE, CLEAR_PG_HEADROOM_REQUEST);
    EXPECT_TRUE(ret_val == true);

    sendClearRequest("USER", CLEAR_PG_SHARED_REQUEST, wm_orch);
    ret_val = validateClearResult(USER_WATERMARKS_TABLE, CLEAR_PG_SHARED_REQUEST);
    EXPECT_TRUE(ret_val == true);

    sendClearRequest("USER", CLEAR_QUEUE_SHARED_UNI_REQUEST, wm_orch);
    ret_val = validateClearResult(USER_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    sendClearRequest("USER", CLEAR_QUEUE_SHARED_MULTI_REQUEST, wm_orch);
    ret_val = validateClearResult(USER_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_MULTI_REQUEST);
    EXPECT_TRUE(ret_val == true);
    clearDB(COUNTERS_DB);

    // PERSISTENT Watermarks Clear Test
    createFakeWmTable(PERSISTENT_WATERMARKS_TABLE, wm_orch);

    sendClearRequest("PERSISTENT", CLEAR_PG_HEADROOM_REQUEST, wm_orch);
    ret_val = validateClearResult(PERSISTENT_WATERMARKS_TABLE, CLEAR_PG_HEADROOM_REQUEST);
    EXPECT_TRUE(ret_val == true);

    sendClearRequest("PERSISTENT", CLEAR_PG_SHARED_REQUEST, wm_orch);
    ret_val = validateClearResult(PERSISTENT_WATERMARKS_TABLE, CLEAR_PG_SHARED_REQUEST);
    EXPECT_TRUE(ret_val == true);

    sendClearRequest("PERSISTENT", CLEAR_QUEUE_SHARED_UNI_REQUEST, wm_orch);
    ret_val = validateClearResult(PERSISTENT_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    sendClearRequest("PERSISTENT", CLEAR_QUEUE_SHARED_MULTI_REQUEST, wm_orch);
    ret_val = validateClearResult(PERSISTENT_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_MULTI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    clearDB(APPL_DB);
    clearDB(COUNTERS_DB);
    clearDB(CONFIG_DB);

    delete wm_orch;
}

/*
 * void WatermarkOrch::doTask(SelectableTimer &timer)
 */
TEST_F(WatermarkTest, TimmerTest)
{
    auto config_db = swss::DBConnector(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    WatermarkOrch *wm_orch = new WatermarkOrch(&config_db, CFG_WATERMARK_TABLE_NAME);
    ASSERT_TRUE(wm_orch != (WatermarkOrch *)NULL);
    bool ret_val = false;

    clearDB(COUNTERS_DB);
    clearDB(CONFIG_DB);

    createFakeWmTable(PERIODIC_WATERMARKS_TABLE, wm_orch);

    // The new interval will be assigned to the timer during the timer handling, 
    // so the orch will reset the interval only when the current timer expires.
    fakeConsumer("SET", "sprtKey", wm_orch);
    EXPECT_TRUE(wm_orch->m_telemetryInterval == NEW_TELEMETRY_INTERVAL);
    int curInterval = 0;

    // Check the interval before the current timer expires.
    SelectableTimer *faketimer = new SelectableTimer(timespec { .tv_sec = 10, .tv_nsec = 0 });
    wm_orch->doTask(*faketimer);
    curInterval = wm_orch->m_telemetryTimer->m_interval.it_interval.tv_sec;
    EXPECT_TRUE(curInterval == DEFAULT_TELEMETRY_INTERVAL);

    // The periodic watermarks should not be cleared before the timer expires.
    ret_val = validateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_PG_HEADROOM_REQUEST);
    EXPECT_TRUE(ret_val == false);

    ret_val = validateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_PG_SHARED_REQUEST);
    EXPECT_TRUE(ret_val == false);

    ret_val = validateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == false);

    ret_val = validateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_MULTI_REQUEST);
    EXPECT_TRUE(ret_val == false);


    // Check the interval when the current timer expires.
    wm_orch->doTask(*wm_orch->m_telemetryTimer);
    curInterval = wm_orch->m_telemetryTimer->m_interval.it_interval.tv_sec;
    EXPECT_TRUE(curInterval == NEW_TELEMETRY_INTERVAL);

    // The periodic watermarks should be cleared by timer!
    ret_val = validateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_PG_HEADROOM_REQUEST);
    EXPECT_TRUE(ret_val == true);

    ret_val = validateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_PG_SHARED_REQUEST);
    EXPECT_TRUE(ret_val == true);

    ret_val = validateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_UNI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    ret_val = validateClearResult(PERIODIC_WATERMARKS_TABLE, CLEAR_QUEUE_SHARED_MULTI_REQUEST);
    EXPECT_TRUE(ret_val == true);

    clearDB(COUNTERS_DB);
    clearDB(CONFIG_DB);

    delete faketimer;
    delete wm_orch;
}
