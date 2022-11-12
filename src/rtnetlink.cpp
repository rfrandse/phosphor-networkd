#include "rtnetlink.hpp"

#include "netlink.hpp"
#include "util.hpp"

#include <linux/rtnetlink.h>

namespace phosphor::network::netlink
{

template <typename Addr>
static std::optional<std::tuple<unsigned, InAddrAny>>
    parse(std::string_view msg)
{
    std::optional<unsigned> ifIdx;
    std::optional<InAddrAny> gw;
    while (!msg.empty())
    {
        auto [hdr, data] = extractRtAttr(msg);
        switch (hdr.rta_type)
        {
            case RTA_OIF:
                ifIdx.emplace(stdplus::raw::copyFromStrict<int>(data));
                break;
            case RTA_GATEWAY:
                gw.emplace(stdplus::raw::copyFromStrict<Addr>(data));
                break;
        }
    }
    if (ifIdx && gw)
    {
        return std::make_tuple(*ifIdx, *gw);
    }
    return std::nullopt;
}

std::optional<std::tuple<unsigned, InAddrAny>>
    gatewayFromRtm(std::string_view msg)
{
    const auto& rtm = extractRtData<rtmsg>(msg);
    if (rtm.rtm_table != RT_TABLE_MAIN || rtm.rtm_dst_len != 0)
    {
        return std::nullopt;
    }
    switch (rtm.rtm_family)
    {
        case AF_INET:
            return parse<in_addr>(msg);
        case AF_INET6:
            return parse<in6_addr>(msg);
    }
    return std::nullopt;
}

AddressInfo addrFromRtm(std::string_view msg)
{
    const auto& ifa = extractRtData<ifaddrmsg>(msg);

    AddressInfo ret;
    ret.ifidx = ifa.ifa_index;
    ret.flags = ifa.ifa_flags;
    ret.scope = ifa.ifa_scope;
    std::optional<InAddrAny> addr;
    while (!msg.empty())
    {
        auto [hdr, data] = extractRtAttr(msg);
        if (hdr.rta_type == IFA_ADDRESS)
        {
            addr.emplace(addrFromBuf(ifa.ifa_family, data));
        }
        else if (hdr.rta_type == IFA_FLAGS)
        {
            ret.flags = stdplus::raw::copyFromStrict<uint32_t>(data);
        }
    }
    if (!addr)
    {
        throw std::runtime_error("Missing address");
    }
    ret.ifaddr = {*addr, ifa.ifa_prefixlen};
    return ret;
}

NeighborInfo neighFromRtm(std::string_view msg)
{
    const auto& ndm = netlink::extractRtData<ndmsg>(msg);

    NeighborInfo ret;
    ret.ifidx = ndm.ndm_ifindex;
    ret.state = ndm.ndm_state;
    while (!msg.empty())
    {
        auto [hdr, data] = netlink::extractRtAttr(msg);
        if (hdr.rta_type == NDA_LLADDR)
        {
            ret.mac = stdplus::raw::copyFrom<ether_addr>(data);
        }
        else if (hdr.rta_type == NDA_DST)
        {
            ret.addr = addrFromBuf(ndm.ndm_family, data);
        }
    }
    return ret;
}

} // namespace phosphor::network::netlink
