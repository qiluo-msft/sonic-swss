#ifndef SWSS_ORCH_H
#define SWSS_ORCH_H

#include <map>
#include <memory>

extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include "dbconnector.h"
#include "table.h"
#include "consumertable.h"
#include "consumerstatetable.h"
#include "notificationconsumer.h"

using namespace std;
using namespace swss;

const char delimiter           = ':';
const char list_item_delimiter = ',';
const char ref_start           = '[';
const char ref_end             = ']';
const char comma               = ',';
const char range_specifier     = '-';

#define MLNX_PLATFORM_SUBSTRING "mellanox"
#define BRCM_PLATFORM_SUBSTRING "broadcom"

#define CONFIGDB_KEY_SEPARATOR "|"
#define DEFAULT_KEY_SEPARATOR  ":"

typedef enum
{
    task_success,
    task_invalid_entry,
    task_failed,
    task_need_retry,
    task_ignore
} task_process_status;

typedef map<string, sai_object_id_t> object_map;
typedef pair<string, sai_object_id_t> object_map_pair;

typedef map<string, object_map*> type_map;
typedef pair<string, object_map*> type_map_pair;
typedef map<string, KeyOpFieldsValuesTuple> SyncMap;

class Orch;

// Design assumption
// 1. one Orch can have one or more ExecutableSelectable
// 2. one ExecutableSelectable must belong to one and only one Orch
// 3. ExecutableSelectable will hold an pointer to new-ed selectable, and delete it during dtor
class ExecutableSelectable : public Selectable
{
public:
    ExecutableSelectable(Selectable *selectable, Orch *orch)
        : m_selectable(selectable)
        , m_orch(orch)
    {
    }

    virtual ~ExecutableSelectable() { delete m_selectable; }

    // Decorating Selectable
    virtual void addFd(fd_set *fd) { return m_selectable->addFd(fd); }
    virtual bool isMe(fd_set *fd) { return m_selectable->isMe(fd); }
    virtual int readCache() { return m_selectable->readCache(); }
    virtual void readMe() { return m_selectable->readMe(); }

    // Disable copying
    ExecutableSelectable(const ExecutableSelectable&) = delete;
    ExecutableSelectable& operator=(const ExecutableSelectable&) = delete;

    // Execute on event happening
    virtual void execute() { }
    virtual void drain() { }

protected:
    Selectable *m_selectable;
    Orch *m_orch;

    // Get the underlying selectable
    Selectable *getSelectable() const { return m_selectable; }
};

class Consumer : public ExecutableSelectable {
public:
    Consumer(TableConsumable *select, Orch *orch)
        : ExecutableSelectable(select, orch)
    {
    }

    TableConsumable *getConsumerTable() const
    {
        return static_cast<TableConsumable *>(getSelectable());
    }

    string getTableName() const
    {
        return getConsumerTable()->getTableName();
    }

    void execute();
    void drain();

    /* Store the latest 'golden' status */
    // TODO: hide?
    SyncMap m_toSync;
};

typedef map<string, std::shared_ptr<ExecutableSelectable>> ConsumerMap;

typedef enum
{
    success,
    field_not_found,
    multiple_instances,
    not_resolved,
    failure
} ref_resolve_status;

typedef pair<DBConnector *, string> TableConnector;
typedef pair<DBConnector *, vector<string>> TablesConnector;

class Orch
{
public:
    Orch(DBConnector *db, const string tableName);
    Orch(DBConnector *db, const vector<string> &tableNames);
    Orch(const vector<TableConnector>& tables);
    virtual ~Orch();

    vector<Selectable*> getSelectables();

    /* Iterate all consumers in m_consumerMap and run doTask(Consumer) */
    void doTask();

    void doTask(ExecutableSelectable &exsel) { exsel.execute(); }
    /* Run doTask against a specific consumer */
    virtual void doTask(Consumer &consumer) = 0;
    virtual void doTask(NotificationConsumer &consumer) { }

    /* TODO: refactor recording */
    static void recordTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
protected:
    ConsumerMap m_consumerMap;

    static void logfileReopen();
    string dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
    ref_resolve_status resolveFieldRefValue(type_map&, const string&, KeyOpFieldsValuesTuple&, sai_object_id_t&);
    bool parseIndexRange(const string &input, sai_uint32_t &range_low, sai_uint32_t &range_high);
    bool parseReference(type_map &type_maps, string &ref, string &table_name, string &object_name);
    ref_resolve_status resolveFieldRefArray(type_map&, const string&, KeyOpFieldsValuesTuple&, vector<sai_object_id_t>&);

    /* Note: consumer will be owned by this class */
    void addConsumer(string tableName, ExecutableSelectable* consumer);
private:
    void addConsumer(DBConnector *db, string tableName);
};

#endif /* SWSS_ORCH_H */
