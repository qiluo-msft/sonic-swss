#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "observer.h"
#include "intfsorch.h"
#include "neighorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "bulker.h"

#include <map>

/* Maximum next hop group number */
#define NHGRP_MAX_SIZE 128

struct NextHopUpdate
{
    IpPrefix prefix;
    IpAddresses nexthopGroup;
};

struct NextHopObserverEntry;

/* RouteTable: destination network, next hop IP address(es) */
typedef std::map<IpPrefix, IpAddresses> RouteTable;
/* NextHopObserverTable: Destination IP address, next hop observer entry */
typedef std::map<IpAddress, NextHopObserverEntry> NextHopObserverTable;

struct NextHopObserverEntry
{
    RouteTable routeTable;
    list<Observer *> observers;
};

class RouteOrch : public Orch, public Subject
{
public:
    RouteOrch(DBConnector *db, string tableName, NeighOrch *neighOrch);

    bool hasNextHopGroup(const IpAddresses& ips) const { return ngh_bulker->hasNextHopGroup(ips); }
    sai_object_id_t getNextHopGroupId(const IpAddresses& ips) const { return ngh_bulker->getNextHopGroupId(ips); }

    void attach(Observer *, const IpAddress&);
    void detach(Observer *, const IpAddress&);

    void increaseNextHopRefCount(const IpAddresses& ips) { return ngh_bulker->increaseNextHopRefCount(ips); }
    void decreaseNextHopRefCount(const IpAddresses& ips) { return ngh_bulker->decreaseNextHopRefCount(ips); }
    bool isRefCounterZero(const IpAddresses& ips) const { return ngh_bulker->isRefCounterZero(ips); }

    bool addNextHopGroup(const IpAddresses&);
    bool removeNextHopGroup(const IpAddresses&);

private:
    RouteBulker m_bulker;
    NextHopGroupBulker *ngh_bulker;
    NeighOrch *m_neighOrch;

    bool m_resync;

    RouteTable m_syncdRoutes;
    NextHopObserverTable m_nextHopObservers;

    void addTempRoute(IpPrefix, IpAddresses);
    bool addRoute(IpPrefix, IpAddresses);
    bool removeRoute(IpPrefix);

    void doTask(Consumer& consumer);

    void notifyNextHopChangeObservers(IpPrefix, IpAddresses, bool);
};

#endif /* SWSS_ROUTEORCH_H */
