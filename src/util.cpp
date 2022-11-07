#include "config.h"

#include "util.hpp"

#include "config_parser.hpp"
#include "types.hpp"

#include <fmt/compile.h>
#include <fmt/format.h>
#include <sys/wait.h>

#include <cctype>
#include <charconv>
#include <fstream>
#ifdef SYNC_MAC_FROM_INVENTORY
#include <nlohmann/json.hpp>
#endif
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <xyz/openbmc_project/Common/error.hpp>

namespace phosphor
{
namespace network
{

using std::literals::string_view_literals::operator""sv;
using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;

namespace internal
{

void executeCommandinChildProcess(stdplus::const_zstring path, char** args)
{
    using namespace std::string_literals;
    pid_t pid = fork();

    if (pid == 0)
    {
        execv(path.c_str(), args);
        exit(255);
    }
    else if (pid < 0)
    {
        auto error = errno;
        log<level::ERR>("Error occurred during fork", entry("ERRNO=%d", error));
        elog<InternalFailure>();
    }
    else if (pid > 0)
    {
        int status;
        while (waitpid(pid, &status, 0) == -1)
        {
            if (errno != EINTR)
            {
                status = -1;
                break;
            }
        }

        if (status < 0)
        {
            fmt::memory_buffer buf;
            fmt::format_to(fmt::appender(buf), "`{}`", path);
            for (size_t i = 0; args[i] != nullptr; ++i)
            {
                fmt::format_to(fmt::appender(buf), " `{}`", args[i]);
            }
            buf.push_back('\0');
            log<level::ERR>("Unable to execute the command",
                            entry("CMD=%s", buf.data()),
                            entry("STATUS=%d", status));
            elog<InternalFailure>();
        }
    }
}

/** @brief Get ignored interfaces from environment */
std::string_view getIgnoredInterfacesEnv()
{
    auto r = std::getenv("IGNORED_INTERFACES");
    if (r == nullptr)
    {
        return "";
    }
    return r;
}

/** @brief Parse the comma separated interface names */
std::unordered_set<std::string_view>
    parseInterfaces(std::string_view interfaces)
{
    std::unordered_set<std::string_view> result;
    while (true)
    {
        auto sep = interfaces.find(',');
        auto interface = interfaces.substr(0, sep);
        while (!interface.empty() && std::isspace(interface.front()))
        {
            interface.remove_prefix(1);
        }
        while (!interface.empty() && std::isspace(interface.back()))
        {
            interface.remove_suffix(1);
        }
        if (!interface.empty())
        {
            result.insert(interface);
        }
        if (sep == interfaces.npos)
        {
            break;
        }
        interfaces = interfaces.substr(sep + 1);
    }
    return result;
}

/** @brief Get the ignored interfaces */
const std::unordered_set<std::string_view>& getIgnoredInterfaces()
{
    static auto ignoredInterfaces = parseInterfaces(getIgnoredInterfacesEnv());
    return ignoredInterfaces;
}

} // namespace internal

void deleteInterface(stdplus::const_zstring intf)
{
    pid_t pid = fork();
    int status{};

    if (pid == 0)
    {

        execl("/sbin/ip", "ip", "link", "delete", "dev", intf.c_str(), nullptr);
        auto error = errno;
        log<level::ERR>("Couldn't delete the device", entry("ERRNO=%d", error),
                        entry("INTF=%s", intf.c_str()));
        elog<InternalFailure>();
    }
    else if (pid < 0)
    {
        auto error = errno;
        log<level::ERR>("Error occurred during fork", entry("ERRNO=%d", error));
        elog<InternalFailure>();
    }
    else if (pid > 0)
    {
        while (waitpid(pid, &status, 0) == -1)
        {
            if (errno != EINTR)
            { /* Error other than EINTR */
                status = -1;
                break;
            }
        }

        if (status < 0)
        {
            log<level::ERR>("Unable to delete the interface",
                            entry("INTF=%s", intf.c_str()),
                            entry("STATUS=%d", status));
            elog<InternalFailure>();
        }
    }
}

std::optional<std::string> interfaceToUbootEthAddr(std::string_view intf)
{
    constexpr auto pfx = "eth"sv;
    if (!intf.starts_with(pfx))
    {
        return std::nullopt;
    }
    intf.remove_prefix(pfx.size());
    auto last = intf.data() + intf.size();
    unsigned long idx;
    auto res = std::from_chars(intf.data(), last, idx);
    if (res.ec != std::errc() || res.ptr != last)
    {
        return std::nullopt;
    }
    if (idx == 0)
    {
        return "ethaddr";
    }
    return fmt::format(FMT_COMPILE("eth{}addr"), idx);
}

static std::optional<DHCPVal> systemdParseDHCP(std::string_view str)
{
    if (config::icaseeq(str, "ipv4"))
    {
        return DHCPVal{.v4 = true, .v6 = false};
    }
    if (config::icaseeq(str, "ipv6"))
    {
        return DHCPVal{.v4 = false, .v6 = true};
    }
    if (auto b = config::parseBool(str); b)
    {
        return DHCPVal{.v4 = *b, .v6 = *b};
    }
    return std::nullopt;
}

inline auto systemdParseLast(const config::Parser& config,
                             std::string_view section, std::string_view key,
                             auto&& fun)
{
    if (auto str = config.map.getLastValueString(section, key); str == nullptr)
    {
        auto err = fmt::format("Unable to get the value of {}[{}] from {}",
                               section, key, config.getFilename().native());
        log<level::NOTICE>(err.c_str(),
                           entry("FILE=%s", config.getFilename().c_str()));
    }
    else if (auto val = fun(*str); !val)
    {
        auto err = fmt::format("Invalid value of {}[{}] from {}: {}", section,
                               key, config.getFilename().native(), *str);
        log<level::NOTICE>(err.c_str(), entry("VALUE=%s", str->c_str()),
                           entry("FILE=%s", config.getFilename().c_str()));
    }
    else
    {
        return val;
    }
    return decltype(fun(std::string_view{}))(std::nullopt);
}

bool getIPv6AcceptRA(const config::Parser& config)
{
#ifdef ENABLE_IPV6_ACCEPT_RA
    constexpr bool def = true;
#else
    constexpr bool def = false;
#endif
    return systemdParseLast(config, "Network", "IPv6AcceptRA",
                            config::parseBool)
        .value_or(def);
}

DHCPVal getDHCPValue(const config::Parser& config)
{
    return systemdParseLast(config, "Network", "DHCP", systemdParseDHCP)
        .value_or(DHCPVal{.v4 = true, .v6 = true});
}

bool getDHCPProp(const config::Parser& config, std::string_view key)
{
    return systemdParseLast(config, "DHCP", key, config::parseBool)
        .value_or(true);
}

namespace mac_address
{

constexpr auto mapperBus = "xyz.openbmc_project.ObjectMapper";
constexpr auto mapperObj = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperIntf = "xyz.openbmc_project.ObjectMapper";
constexpr auto propIntf = "org.freedesktop.DBus.Properties";
constexpr auto methodGet = "Get";
constexpr auto configFile = "/usr/share/network/config.json";

using DbusObjectPath = std::string;
using DbusService = std::string;
using DbusInterface = std::string;
using ObjectTree =
    std::map<DbusObjectPath, std::map<DbusService, std::vector<DbusInterface>>>;

constexpr auto invBus = "xyz.openbmc_project.Inventory.Manager";
constexpr auto invNetworkIntf =
    "xyz.openbmc_project.Inventory.Item.NetworkInterface";
constexpr auto invRoot = "/xyz/openbmc_project/inventory";

ether_addr getfromInventory(sdbusplus::bus_t& bus, const std::string& intfName)
{

    std::string interfaceName = intfName;

#ifdef SYNC_MAC_FROM_INVENTORY
    // load the config JSON from the Read Only Path
    std::ifstream in(configFile);
    nlohmann::json configJson;
    in >> configJson;
    interfaceName = configJson[intfName];
#endif

    std::vector<DbusInterface> interfaces;
    interfaces.emplace_back(invNetworkIntf);

    auto depth = 0;

    auto mapperCall =
        bus.new_method_call(mapperBus, mapperObj, mapperIntf, "GetSubTree");

    mapperCall.append(invRoot, depth, interfaces);

    auto mapperReply = bus.call(mapperCall);
    if (mapperReply.is_method_error())
    {
        log<level::ERR>("Error in mapper call");
        elog<InternalFailure>();
    }

    ObjectTree objectTree;
    mapperReply.read(objectTree);

    if (objectTree.empty())
    {
        log<level::ERR>("No Object has implemented the interface",
                        entry("INTERFACE=%s", invNetworkIntf));
        elog<InternalFailure>();
    }

    DbusObjectPath objPath;
    DbusService service;

    if (1 == objectTree.size())
    {
        objPath = objectTree.begin()->first;
        service = objectTree.begin()->second.begin()->first;
    }
    else
    {
        // If there are more than 2 objects, object path must contain the
        // interface name
        for (auto const& object : objectTree)
        {
            log<level::INFO>("interface",
                             entry("INT=%s", interfaceName.c_str()));
            log<level::INFO>("object", entry("OBJ=%s", object.first.c_str()));

            if (std::string::npos != object.first.find(interfaceName.c_str()))
            {
                objPath = object.first;
                service = object.second.begin()->first;
                break;
            }
        }

        if (objPath.empty())
        {
            log<level::ERR>("Can't find the object for the interface",
                            entry("intfName=%s", interfaceName.c_str()));
            elog<InternalFailure>();
        }
    }

    auto method = bus.new_method_call(service.c_str(), objPath.c_str(),
                                      propIntf, methodGet);

    method.append(invNetworkIntf, "MACAddress");

    auto reply = bus.call(method);
    if (reply.is_method_error())
    {
        log<level::ERR>("Failed to get MACAddress",
                        entry("PATH=%s", objPath.c_str()),
                        entry("INTERFACE=%s", invNetworkIntf));
        elog<InternalFailure>();
    }

    std::variant<std::string> value;
    reply.read(value);
    return ToAddr<ether_addr>{}(std::get<std::string>(value));
}

bool isEmpty(const ether_addr& mac)
{
    return mac == ether_addr{};
}

bool isMulticast(const ether_addr& mac)
{
    return mac.ether_addr_octet[0] & 0b1;
}

bool isUnicast(const ether_addr& mac)
{
    return !isEmpty(mac) && !isMulticast(mac);
}

} // namespace mac_address
} // namespace network
} // namespace phosphor
