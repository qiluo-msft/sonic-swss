#include <assert.h>
#include <deque>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <boost/functional/hash.hpp>
#include <sairedis.h>
#include "sai.h"

static inline bool operator==(const sai_ip_prefix_t& a, const sai_ip_prefix_t& b)
{
    if (a.addr_family != b.addr_family) return false;

    if (a.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        return a.addr.ip4 == b.addr.ip4
            && a.mask.ip4 == b.mask.ip4
            ;
    }
    else if (a.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
    {
        return memcmp(a.addr.ip6, b.addr.ip6, sizeof(a.addr.ip6)) == 0
            && memcmp(a.mask.ip6, b.mask.ip6, sizeof(a.mask.ip6)) == 0
            ;
    }
    else
    {
        throw std::invalid_argument("a has invalid addr_family");
    }
}

static inline bool operator==(const sai_route_entry_t& a, const sai_route_entry_t& b)
{
    return a.switch_id == b.switch_id
        && a.vr_id == b.vr_id
        && a.destination == b.destination
        ;
}

static inline std::size_t hash_value(const sai_ip_prefix_t& a)
{
    size_t seed = 0;
    boost::hash_combine(seed, a.addr_family);
    if (a.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        boost::hash_combine(seed, a.addr.ip4);
        boost::hash_combine(seed, a.mask.ip4);
    }
    else if (a.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
    {
        boost::hash_combine(seed, a.addr.ip6);
        boost::hash_combine(seed, a.mask.ip6);
    }
    return seed;
}

namespace std
{
    template<>
    struct hash<sai_route_entry_t>
    {
        size_t operator()(const sai_route_entry_t& a) const noexcept
        {
            size_t seed = 0;
            boost::hash_combine(seed, a.switch_id);
            boost::hash_combine(seed, a.vr_id);
            boost::hash_combine(seed, a.destination);
            return seed;
        }
    };
}

class RouteBulker
{
public:
    RouteBulker(sai_route_api_t *sai_route_api, sai_next_hop_group_api_t* sai_next_hop_group_api)
    {
        route_api = sai_route_api;
        next_hop_group_api = sai_next_hop_group_api;
    }

    sai_status_t create_route_entry(
        _In_ const sai_route_entry_t *route_entry,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        creating_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*route_entry),
                std::forward_as_tuple(attr_list, attr_list + attr_count));
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t remove_route_entry(
        _In_ const sai_route_entry_t *route_entry)
    {
        assert(route_entry);
        if (!route_entry) throw std::invalid_argument("route_entry is null");

        auto found_setting = setting_entries.find(*route_entry);
        if (found_setting != setting_entries.end())
        {
            setting_entries.erase(found_setting);
        }

        auto found_creating = creating_entries.find(*route_entry);
        if (found_creating != creating_entries.end())
        {
            creating_entries.erase(found_creating);
        }
        else
        {
            removing_entries.emplace(*route_entry);
        }

        return SAI_STATUS_SUCCESS;
    }

    sai_status_t set_route_entry_attribute(
        _In_ const sai_route_entry_t *route_entry,
        _In_ const sai_attribute_t *attr)
    {
        auto found_setting = setting_entries.find(*route_entry);
        if (found_setting != setting_entries.end())
        {
            // For simplicity, just insert new attribute at the vector end, no merging
            found_setting->second.emplace_back(*attr);
        }
        else
        {
            // Create a new key if not exists in the map
            setting_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*route_entry),
                std::forward_as_tuple(1, *attr));
        }

        return SAI_STATUS_SUCCESS;
    }

    void flush()
    {
        // Removing
        if (!removing_entries.empty())
        {
            for (auto i: removing_entries)
            {
                auto& route_entry = i;
                route_api->remove_route_entry(&route_entry);
            }
            removing_entries.clear();
        }

        // Creating
        if (!creating_entries.empty())
        {
            vector<sai_route_entry_t> rs;
            vector<sai_attribute_t const*> tss;
            vector<uint32_t> cs;

            for (auto const& i: creating_entries)
            {
                auto const& route_entry = i.first;
                auto const& attrs = i.second;

                rs.push_back(route_entry);
                tss.push_back(attrs.data());
                cs.push_back((uint32_t)attrs.size());
            }
            uint32_t route_count = (uint32_t)creating_entries.size();
            vector<sai_status_t> statuses(route_count);
            sai_bulk_create_route_entry(route_count, rs.data(), cs.data(), tss.data()
                , SAI_BULK_OP_TYPE_INGORE_ERROR, statuses.data());

            creating_entries.clear();
        }

        // Setting
        if (!setting_entries.empty())
        {
            vector<sai_route_entry_t> rs;
            vector<sai_attribute_t> ts;
            vector<uint32_t> cs;

            for (auto const& i: setting_entries)
            {
                auto const& route_entry = i.first;
                auto const& attrs = i.second;
                for (auto const& attr: attrs)
                {
                    rs.push_back(route_entry);
                    ts.push_back(attr);
                    //route_api->set_route_entry_attribute(&route_entry, &attr);
                }
            }
            uint32_t route_count = (uint32_t)setting_entries.size();
            vector<sai_status_t> statuses(route_count);
            sai_bulk_set_route_entry_attribute(route_count, rs.data(), ts.data()
                , SAI_BULK_OP_TYPE_INGORE_ERROR, statuses.data());

            setting_entries.clear();
        }
    }

    void clear()
    {
        removing_entries.clear();
        creating_entries.clear();
        setting_entries.clear();
    }

private:
    sai_route_api_t *                                                       route_api;
    sai_next_hop_group_api_t *                                              next_hop_group_api;
    std::unordered_map<sai_route_entry_t, std::vector<sai_attribute_t>>     creating_entries;
    std::unordered_map<sai_route_entry_t, std::vector<sai_attribute_t>>     setting_entries;
    std::unordered_set<sai_route_entry_t>                                   removing_entries;
};
