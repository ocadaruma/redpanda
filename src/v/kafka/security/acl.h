/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */
#pragma once
#include "kafka/types.h"
#include "model/fundamental.h"
#include "seastarx.h"
#include "utils/named_type.h"

#include <seastar/core/sstring.hh>
#include <seastar/net/inet_address.hh>

#include <absl/container/btree_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/core.h>

namespace kafka {

namespace details {
template<class T>
struct dependent_false : std::false_type {};
} // namespace details

// cluster is a resource type and the acl data model requires that resources
// have names, so this is a fixed name for that resource.
using acl_cluster_name = named_type<ss::sstring, struct acl_cluster_name_type>;
inline const acl_cluster_name default_cluster_name("redpanda-cluster");

/*
 * An ACL resource type.
 */
enum class resource_type {
    topic,
    group,
    cluster,
    transactional_id,
};

template<typename T>
inline resource_type get_resource_type() {
    if constexpr (std::is_same_v<T, model::topic>) {
        return resource_type::topic;
    } else if constexpr (std::is_same_v<T, kafka::group_id>) {
        return resource_type::group;
    } else if constexpr (std::is_same_v<T, kafka::acl_cluster_name>) {
        return resource_type::cluster;
    } else if constexpr (std::is_same_v<T, kafka::transactional_id>) {
        return resource_type::transactional_id;
    } else {
        static_assert(details::dependent_false<T>::value, "Unsupported type");
    }
}

/*
 * A pattern rule for matching ACL resource names.
 */
enum class pattern_type {
    literal,
    prefixed,
};

/*
 * an operation on a resource.
 */
enum class acl_operation {
    all,
    read,
    write,
    create,
    remove,
    alter,
    describe,
    cluster_action,
    describe_configs,
    alter_configs,
    idempotent_write,
};

/*
 * Compute the implied operations based on the specified operation.
 */
inline std::vector<acl_operation> acl_implied_ops(acl_operation operation) {
    switch (operation) {
    case acl_operation::describe:
        return {
          acl_operation::describe,
          acl_operation::read,
          acl_operation::write,
          acl_operation::remove,
          acl_operation::alter,
        };
    case acl_operation::describe_configs:
        return {
          acl_operation::describe_configs,
          acl_operation::alter_configs,
        };
    default:
        return {operation};
    }
}

/*
 * Grant or deny access.
 */
enum class acl_permission {
    deny,
    allow,
};

/*
 * Principal type
 *
 * Only `User` is currently supported, but when integrating with other identity
 * providers it may be useful to introduce a `Group` type.
 */
enum class principal_type {
    user,
};

inline std::ostream& operator<<(std::ostream& os, resource_type type) {
    switch (type) {
    case resource_type::topic:
        return os << "topic";
    case resource_type::group:
        return os << "group";
    case resource_type::cluster:
        return os << "cluster";
    case resource_type::transactional_id:
        return os << "transactional_id";
    }
}

inline std::ostream& operator<<(std::ostream& os, pattern_type type) {
    switch (type) {
    case pattern_type::literal:
        return os << "literal";
    case pattern_type::prefixed:
        return os << "prefixed";
    }
}

/*
 * Kafka principal is (principal-type, principal)
 */
class acl_principal {
public:
    acl_principal(principal_type type, ss::sstring name)
      : _type(type)
      , _name(std::move(name)) {}

    friend bool operator==(const acl_principal&, const acl_principal&)
      = default;

    template<typename H>
    friend H AbslHashValue(H h, const acl_principal& e) {
        return H::combine(std::move(h), e._type, e._name);
    }

    bool wildcard() const { return _name == "*"; }

private:
    principal_type _type;
    ss::sstring _name;
};

inline const acl_principal acl_wildcard_user(principal_type::user, "*");

/*
 * Resource pattern matches resources using a (type, name, pattern) tuple. The
 * pattern type changes how matching occurs (e.g. literal, name prefix).
 */
class resource_pattern {
public:
    static constexpr const char* wildcard = "*";

    resource_pattern(resource_type type, ss::sstring name, pattern_type pattern)
      : _resource(type)
      , _name(std::move(name))
      , _pattern(pattern) {}

    friend bool operator==(const resource_pattern&, const resource_pattern&)
      = default;

    template<typename H>
    friend H AbslHashValue(H h, const resource_pattern& e) {
        return H::combine(std::move(h), e._resource, e._name, e._pattern);
    }

    friend std::ostream& operator<<(std::ostream&, const resource_pattern&);

    resource_type resource() const { return _resource; }
    const ss::sstring& name() const { return _name; }
    pattern_type pattern() const { return _pattern; }

private:
    resource_type _resource;
    ss::sstring _name;
    pattern_type _pattern;
};

inline std::ostream& operator<<(std::ostream& os, const resource_pattern& r) {
    fmt::print(
      os,
      "type {{{}}} name {{{}}} pattern {{{}}}",
      r._resource,
      r._name,
      r._pattern);
    return os;
}

/*
 * A host (or wildcard) in an ACL rule.
 */
class acl_host {
public:
    explicit acl_host(const ss::sstring& host)
      : _addr(host) {}

    explicit acl_host(ss::net::inet_address host)
      : _addr(host) {}

    static acl_host wildcard_host() { return acl_host{}; }

    friend bool operator==(const acl_host&, const acl_host&) = default;

    template<typename H>
    friend H AbslHashValue(H h, const acl_host& host) {
        if (host._addr) {
            return H::combine(std::move(h), *host._addr);
        } else {
            return H::combine(std::move(h), ss::net::inet_address{});
        }
    }

private:
    acl_host() = default;

    std::optional<ss::net::inet_address> _addr;
};

inline const acl_host acl_wildcard_host = acl_host::wildcard_host();

/*
 * An ACL entry specifies if a principal (connected from a specific host) is
 * permitted to execute an operation on. When associated with a resource, it
 * describes if the principal can execute the operation on that resource.
 */
class acl_entry {
public:
    acl_entry(
      acl_principal principal,
      acl_host host,
      acl_operation operation,
      acl_permission permission)
      : _principal(std::move(principal))
      , _host(host)
      , _operation(operation)
      , _permission(permission) {}

    friend bool operator==(const acl_entry&, const acl_entry&) = default;

    template<typename H>
    friend H AbslHashValue(H h, const acl_entry& e) {
        return H::combine(
          std::move(h), e._principal, e._host, e._operation, e._permission);
    }

    const acl_principal& principal() const { return _principal; }
    const acl_host& host() const { return _host; }
    acl_operation operation() const { return _operation; }
    acl_permission permission() const { return _permission; }

private:
    acl_principal _principal;
    acl_host _host;
    acl_operation _operation;
    acl_permission _permission;
};

/*
 * An ACL binding is an association of resource(s) and an ACL entry. An ACL
 * binding describes if a principal may access resources.
 */
class acl_binding {
public:
    acl_binding(resource_pattern pattern, acl_entry entry)
      : _pattern(std::move(pattern))
      , _entry(std::move(entry)) {}

    friend bool operator==(const acl_binding&, const acl_binding&) = default;

    template<typename H>
    friend H AbslHashValue(H h, const acl_binding& e) {
        return H::combine(std::move(h), e._pattern, e._entry);
    }

    const resource_pattern& pattern() const { return _pattern; }
    const acl_entry& entry() const { return _entry; }

private:
    resource_pattern _pattern;
    acl_entry _entry;
};

/*
 * A filter for matching resources.
 */
class resource_pattern_filter {
public:
    struct pattern_match {};
    using pattern_filter_type = std::variant<pattern_type, pattern_match>;

    resource_pattern_filter(
      std::optional<resource_type> type,
      std::optional<ss::sstring> name,
      std::optional<pattern_filter_type> pattern)
      : _resource(type)
      , _name(std::move(name))
      , _pattern(pattern) {}

    // NOLINTNEXTLINE(hicpp-explicit-conversions)
    resource_pattern_filter(const resource_pattern& resource)
      : resource_pattern_filter(
        resource.resource(), resource.name(), resource.pattern()) {}

    /*
     * A filter that matches any resource.
     */
    static const resource_pattern_filter& any() {
        static const resource_pattern_filter filter(
          std::nullopt, std::nullopt, std::nullopt);
        return filter;
    }

    bool matches(const resource_pattern& pattern) const;

private:
    std::optional<resource_type> _resource;
    std::optional<ss::sstring> _name;
    std::optional<pattern_filter_type> _pattern;
};

inline bool
resource_pattern_filter::matches(const resource_pattern& pattern) const {
    if (_resource && *_resource != pattern.resource()) {
        return false;
    }

    if (
      _pattern && std::holds_alternative<pattern_type>(*_pattern)
      && std::get<pattern_type>(*_pattern) != pattern.pattern()) {
        return false;
    }

    if (!_name) {
        return true;
    }

    if (
      !_pattern || (std::holds_alternative<pattern_type>(*_pattern)
      && std::get<pattern_type>(*_pattern) == pattern.pattern())) {
        return _name == pattern.name();
    }

    switch (pattern.pattern()) {
    case pattern_type::literal:
        return _name == pattern.name()
               || pattern.name() == resource_pattern::wildcard;

    case pattern_type::prefixed:
        return std::string_view(*_name).starts_with(pattern.name());
    }

    __builtin_unreachable();
}

/*
 * A filter for matching ACL entries.
 */
class acl_entry_filter {
public:
    // NOLINTNEXTLINE(hicpp-explicit-conversions)
    acl_entry_filter(const acl_entry& entry)
      : acl_entry_filter(
        entry.principal(),
        entry.host(),
        entry.operation(),
        entry.permission()) {}

    acl_entry_filter(
      std::optional<acl_principal> principal,
      std::optional<acl_host> host,
      std::optional<acl_operation> operation,
      std::optional<acl_permission> permission)
      : _principal(std::move(principal))
      , _host(std::move(host))
      , _operation(operation)
      , _permission(permission) {}

    /*
     * A filter that matches any ACL entry.
     */
    static const acl_entry_filter& any() {
        static const acl_entry_filter filter(
          std::nullopt, std::nullopt, std::nullopt, std::nullopt);
        return filter;
    }

    bool matches(const acl_entry& other) const;

private:
    std::optional<acl_principal> _principal;
    std::optional<acl_host> _host;
    std::optional<acl_operation> _operation;
    std::optional<acl_permission> _permission;
};

inline bool acl_entry_filter::matches(const acl_entry& other) const {
    if (_principal && _principal != other.principal()) {
        return false;
    }

    if (_host && _host != other.host()) {
        return false;
    }

    if (_operation && *_operation != other.operation()) {
        return false;
    }

    return !_permission || *_permission == other.permission();
}

/*
 * A filter for matching ACL bindings.
 */
class acl_binding_filter {
public:
    acl_binding_filter(resource_pattern_filter pattern, acl_entry_filter acl)
      : _pattern(std::move(pattern))
      , _acl(std::move(acl)) {}

    /*
     * A filter that matches any ACL binding.
     */
    static const acl_binding_filter& any() {
        static const acl_binding_filter filter(
          resource_pattern_filter::any(), acl_entry_filter::any());
        return filter;
    }

    bool matches(const acl_binding& binding) const {
        return _pattern.matches(binding.pattern())
               && _acl.matches(binding.entry());
    }

    const resource_pattern_filter& pattern() const { return _pattern; }
    const acl_entry_filter& entry() const { return _acl; }

private:
    resource_pattern_filter _pattern;
    acl_entry_filter _acl;
};

} // namespace kafka
