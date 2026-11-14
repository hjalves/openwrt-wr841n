# JSON-RPC and ubus methods on this router

Read-only snapshot of `192.168.0.254` on 2026-11-14. Objects can appear or disappear as services start or stop; names and signatures may change with configuration or firmware. This list was collected with `ubus -v list` over SSH and by querying the existing HTTP bridge. Listing a method does **not** mean an HTTP session is authorized to call it.

## The two JSON-RPC operations

The installed uhttpd ubus bridge exposes these top-level JSON-RPC `method` values at `POST /ubus`:

| JSON-RPC method | Meaning |
|---|---|
| `list` | List ubus objects, or list the methods and advertised signatures of specified objects. |
| `call` | Invoke a method on an ubus object, subject to session ACLs. |

For example, this lists objects. On this router it was accessible without authentication:

```sh
curl -sS http://192.168.0.254/ubus \
  -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"list"}'
```

To inspect a particular object's advertised methods:

```sh
curl -sS http://192.168.0.254/ubus \
  -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"list","params":["system"]}'
```

`call` takes `[session_id, object, method, arguments]`. An all-zero session represents the anonymous session:

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "call",
  "params": [
    "00000000000000000000000000000000",
    "session",
    "access",
    {"scope": "ubus", "object": "system", "function": "info"}
  ]
}
```

At the snapshot this safe permission check returned `[0,{"access":false}]`. A successful ubus call typically returns `"result":[0,{...}]`, where `0` is the ubus status. Authorization or protocol failures may instead appear as a JSON-RPC `error` even with HTTP status 200. `session.login` can issue a token; see [`README.md`](README.md) for the SSH-tunneled example and ACL requirements.

The advertised parameter types below come from `ubus -v list`. They are not a complete specification of required fields, allowed values, return data, or side effects. An empty signature means no parameters were **advertised**, not necessarily that the method is safe or accepts no other fields.

## Discovered ubus objects and methods

### `dhcp`

| Method | Advertised parameters |
|---|---|
| `ipv4leases` | none |
| `ipv6leases` | none |

### `hostapd.wlan0`

| Method | Advertised parameters |
|---|---|
| `get_clients` | none |
| `del_client` | `addr`: string, `reason`: integer, `deauth`: boolean, `ban_time`: integer |
| `list_bans` | none |
| `wps_start` | none |
| `wps_cancel` | none |
| `update_beacon` | none |
| `switch_chan` | `freq`: integer, `bcn_count`: integer |
| `set_vendor_elements` | `vendor_elements`: string |

### `log`

| Method | Advertised parameters |
|---|---|
| `read` | `lines`: integer |
| `write` | `event`: string |

### `network`

| Method | Advertised parameters |
|---|---|
| `restart` | none |
| `reload` | none |
| `add_host_route` | `target`: string, `v6`: boolean, `interface`: string |
| `get_proto_handlers` | none |
| `add_dynamic` | `name`: string |

### `network.device`

| Method | Advertised parameters |
|---|---|
| `status` | `name`: string |
| `set_alias` | `alias`: array, `device`: string |
| `set_state` | `name`: string, `defer`: boolean |

### `network.interface` objects

The following four objects advertised the same method names and signatures:

- `network.interface`
- `network.interface.lan`
- `network.interface.loopback`
- `network.interface.wan`

| Method | Advertised parameters |
|---|---|
| `up`, `down`, `status`, `prepare`, `dump` | none |
| `add_device`, `remove_device` | `name`: string, `link-ext`: boolean |
| `notify_proto`, `remove`, `set_data` | none |

### `network.wireless`

`up`, `down`, `status`, `notify`, and `get_validate`; no parameters were advertised for these methods.

### `service`

| Method | Advertised parameters |
|---|---|
| `set`, `add` | `name`: string, `script`: string, `instances`: table, `triggers`: array, `validate`: array |
| `list` | `name`: string, `verbose`: boolean |
| `delete` | `name`: string, `instance`: string |
| `update_start`, `update_complete` | `name`: string |
| `event` | `type`: string, `data`: table |
| `validate` | `package`: string, `type`: string, `service`: string |
| `get_data` | `name`: string, `instance`: string, `type`: string |

### `session`

| Method | Advertised parameters |
|---|---|
| `create` | `timeout`: integer |
| `list` | `ubus_rpc_session`: string |
| `grant`, `revoke` | `ubus_rpc_session`: string, `scope`: string, `objects`: array |
| `access` | `ubus_rpc_session`: string, `scope`: string, `object`: string, `function`: string |
| `set` | `ubus_rpc_session`: string, `values`: table |
| `get` | `ubus_rpc_session`: string, `keys`: array |
| `unset` | `ubus_rpc_session`: string, `keys`: array |
| `destroy` | `ubus_rpc_session`: string |
| `login` | `username`: string, `password`: string, `timeout`: integer |

On this router, the observed anonymous ACL allowed `session.login` and `session.access`. Most other calls need an appropriate session ACL. During a previous check, a root login succeeded but a call to `system.info` was denied because its session did not have that object/method permission. Permissions can change when ACLs are configured.

### `system`

| Method | Advertised parameters |
|---|---|
| `board`, `info`, `upgrade`, `nandupgrade` | `nandupgrade`: `path` string; no fields advertised for the others |
| `watchdog` | `frequency`: integer, `timeout`: integer, `stop`: boolean |
| `signal` | `pid`: integer, `signum`: integer |

### `uci`

| Method | Advertised parameters |
|---|---|
| `configs` | none |
| `get`, `state` | `config`, `section`, `option`, `type`: strings; `match`: table; `ubus_rpc_session`: string |
| `add` | `config`, `type`, `name`: strings; `values`: table; `ubus_rpc_session`: string |
| `set` | `config`, `section`, `type`: strings; `match`, `values`: tables; `ubus_rpc_session`: string |
| `delete` | `config`, `section`, `type`, `option`: strings; `match`: table; `options`: array; `ubus_rpc_session`: string |
| `rename` | `config`, `section`, `option`, `name`, `ubus_rpc_session`: strings |
| `order` | `config`: string, `sections`: array, `ubus_rpc_session`: string |
| `changes`, `revert`, `commit` | `config`, `ubus_rpc_session`: strings |
| `apply` | `rollback`: boolean, `timeout`: integer, `ubus_rpc_session`: string |
| `confirm`, `rollback` | `ubus_rpc_session`: string |
| `reload_config` | none |

## Custom example

When this directory's `ubus-hello` process is running, it adds `example.hello` with a `say` method. It was not running during the current snapshot, so it is absent from the list above. HTTP access requires a matching RPC ACL; see [`README.md`](README.md).

## Safety

Many methods are **not read-only**. For example, `network.restart`, interface `down`, wireless `down`, service `delete`, `uci.commit`/`apply`, `system.upgrade`, and `system.watchdog` can interrupt access, change configuration, or damage service availability. Do not invoke them merely to explore. UCI and status methods can also expose sensitive configuration.

The HTTP bridge on this router is unencrypted, and unauthenticated object listing was observed. Use an SSH tunnel or trusted management network for passwords and session tokens, and grant the minimum ACL needed. No state-changing ubus methods were called during this inventory.
