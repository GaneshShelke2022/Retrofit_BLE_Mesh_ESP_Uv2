# BLE Mesh Schedule Management Protocol (Phase 2)

## 1. Overview & Architecture

The **Schedule Management Subsystem** allows a Flutter client application or network provisioner to create, query, modify, delete, and test automated time-based schedules on an ESP32-C3 BLE Mesh **Master** node.

All schedule transactions are conveyed **over the BLE Mesh network** via a dedicated **Vendor Server Model** hosted on the master's primary element (Element 0). Schedules are stored in non-volatile flash (NVS) under the namespace `"lumflux_sched"` and executed automatically by the scheduler engine when the system clock matches the schedule's trigger time in India Standard Time (IST, UTC+5:30).

```
+---------------------+                      +-----------------------------+
| Flutter Mobile App  |                      | ESP32-C3 Master Node        |
| (BLE Mesh Client)   |                      |                             |
|                     |                      |  +-----------------------+  |
|                     |                      |  | Element 0: Primary    |  |
|                     |                      |  |  - Config Server      |  |
|                     |                      |  |  - Health Server      |  |
|                     |                      |  |  - Gen OnOff Server 0 |  |
|                     |                      |  |  - Gen PowerOnOff 0   |  |
|                     |  Mesh Vendor Msg     |  |  - Vendor Sched Server|  |
|                     | -------------------> |  +-----------------------+  |
|                     | (3-byte Vnd Opcodes) |              |              |
|                     |                      |              v              |
|                     |                      |   [Worker Task Queue]       |
|                     |                      |              |              |
|                     |                      |              v              |
|                     |                      |     [Staging Buffer]        |
|                     |                      |     (CRC32 Validation)      |
|                     |                      |              |              |
|                     |                      |              v              |
|                     |                      |     [NVS Flash Storage]     |
|                     |                      |     ("lumflux_sched")       |
|                     |                      |              |              |
|                     |                      |              v              |
|                     |                      |    [Scheduler Engine Task]  |
|                     |                      |    (IST Minute Evaluator)   |
|                     |                      |              |              |
|                     |                      |              v              |
|                     |                      |   [Relay HW / Local Lamps]  |
+---------------------+                      +-----------------------------+
```

---

## 2. Model & Addressing Parameters

| Parameter | Value | Description |
| :--- | :--- | :--- |
| **Element** | Element 0 (Primary Element) | Shared with primary Generic OnOff Server and Config Server |
| **Company ID** | `0x02E5` (Espressif Systems) | Configurable via `CONFIG_SCHED_VENDOR_COMPANY_ID` |
| **Model ID** | `0x0001` | Configurable via `CONFIG_SCHED_VENDOR_MODEL_ID` |
| **Encryption** | AppKey-secured | Client must bind current network AppKey to Model ID `0x0001` |
| **Publication** | Buffer: 3 + 64 bytes, Role: `ROLE_NODE` | Supports unsegmented and segmented mesh status replies |
| **Rate Limit** | Max 8 messages / second | Excess incoming messages are dropped with a warning |

---

## 3. Vendor Opcode Table

All vendor opcodes follow the standard Bluetooth SIG 3-byte vendor opcode encoding:
`Opcode = 0xC00000 | (OpID << 16) | CompanyID` (Little-endian on wire).

| Opcode ID | 3-Byte Opcode (CID `0x02E5`) | Name | Direction | Min Payload | Description |
| :---: | :---: | :--- | :---: | :---: | :--- |
| `0x01` | `0xC102E5` | `SCHED_VND_OP_BEGIN` | App -> Node | 29 bytes | Initializes chunked upload staging for schedule metadata |
| `0x02` | `0xC202E5` | `SCHED_VND_OP_ACTIONS` | App -> Node | 3 bytes | Appends a chunk of 1 to 4 actions to the staging buffer |
| `0x03` | `0xC302E5` | `SCHED_VND_OP_COMMIT` | App -> Node | 5 bytes | Verifies CRC32 and commits staged schedule to NVS |
| `0x04` | `0xC402E5` | `SCHED_VND_OP_DELETE` | App -> Node | 2 bytes | Deletes schedule by ID from NVS |
| `0x05` | `0xC502E5` | `SCHED_VND_OP_ENABLE` | App -> Node | 3 bytes | Enables or disables an existing schedule |
| `0x06` | `0xC602E5` | `SCHED_VND_OP_CLEAR` | App -> Node | 1 byte | Erases all stored schedules from NVS |
| `0x07` | `0xC702E5` | `SCHED_VND_OP_GET` | App -> Node | 2 bytes | Queries a single schedule by ID |
| `0x08` | `0xC802E5` | `SCHED_VND_OP_LIST` | App -> Node | 1 byte | Queries count and 16-bit active schedule bitmask |
| `0x09` | `0xC902E5` | `SCHED_VND_OP_RUN_NOW` | App -> Node | 2 bytes | Immediately executes schedule actions for verification |
| `0x0A` | `0xCA02E5` | `SCHED_VND_OP_TIME_GET` | App -> Node | 1 byte | Queries NTP sync status, IST epoch, and slot capacity |
| `0x0B` | `0xCB02E5` | `SCHED_VND_OP_STATUS` | Node -> App | 3 bytes | Generic operation acknowledge / error reply |
| `0x0C` | `0xCC02E5` | `SCHED_VND_OP_LIST_STATUS`| Node -> App | 4 bytes | Reply to `SCHED_LIST` with count and active bitmask |
| `0x0D` | `0xCD02E5` | `SCHED_VND_OP_TIME_STATUS`| Node -> App | 11 bytes | Reply to `TIME_GET` with time status and capacity |

---

## 4. Wire Protocol Message Formats

All multi-byte integer fields (`uint16_t`, `uint32_t`, `int16_t`) are encoded in **Little-Endian** byte order.

### 4.1. `SCHED_VND_OP_BEGIN` (Upload Header)
**Payload Length:** 29 bytes

```
Byte:   0     1     2     3     4     5     6     7     8..9    10    11    12..27     28
     +-----+-----+-----+-----+-----+-----+-----+-----+-------+-----+-----+---------+-----+
     | TID | ID  | VER | EN  | HR  | MIN | REP | DYS | YEAR  | MON | DAY |  NAME   | CNT |
     +-----+-----+-----+-----+-----+-----+-----+-----+-------+-----+-----+---------+-----+
```

| Offset | Field | Type | Description |
| :---: | :--- | :--- | :--- |
| 0 | `TID` | `uint8_t` | Transaction ID (chosen by client, echoed in replies) |
| 1 | `ID` | `uint8_t` | Schedule ID (`1` to `16`) |
| 2 | `VER` | `uint8_t` | Protocol Version (must be `1`) |
| 3 | `EN` | `uint8_t` | Enabled flag (`1` = active, `0` = disabled) |
| 4 | `HR` | `uint8_t` | Hour (`0` to `23` IST) |
| 5 | `MIN` | `uint8_t` | Minute (`0` to `59` IST) |
| 6 | `REP` | `uint8_t` | Repeat type (`0` = ONCE, `1` = DAILY, `2` = WEEKDAYS, `3` = WEEKENDS, `4` = CUSTOM) |
| 7 | `DYS` | `uint8_t` | Repeat days bitmask for CUSTOM (`bit 0`=Sun, `bit 1`=Mon ... `bit 6`=Sat) |
| 8..9 | `YEAR`| `uint16_t`| Calendar year for ONCE repeat (e.g., `2026`) |
| 10 | `MON` | `uint8_t` | Calendar month for ONCE repeat (`1` to `12`) |
| 11 | `DAY` | `uint8_t` | Calendar day of month for ONCE repeat (`1` to `31`) |
| 12..27| `NAME`| `char[16]` | Null-padded UTF-8 / ASCII human-readable label |
| 28 | `CNT` | `uint8_t` | Total expected action count (`1` to `8`) |

---

### 4.2. `SCHED_VND_OP_ACTIONS` (Upload Chunk)
**Payload Length:** `3 + (N * 4)` bytes (e.g., 11 bytes for 2 actions)

```
Byte:   0     1     2     3..4    5     6     7..8    9     10
     +-----+-----+-----+-------+-----+-----+-------+-----+-----+
     | TID | SEQ | CNT | TGT 0 | KND | VAL | TGT 1 | KND | VAL |
     +-----+-----+-----+-------+-----+-----+-------+-----+-----+
```

| Offset | Field | Type | Description |
| :---: | :--- | :--- | :--- |
| 0 | `TID` | `uint8_t` | Transaction ID matching `SCHED_BEGIN` |
| 1 | `SEQ` | `uint8_t` | Action chunk sequence number (0-indexed) |
| 2 | `CNT` | `uint8_t` | Number of actions in this message (`N` = 1..4) |
| *Repeated:* | | | |
| +0..+1 | `TGT` | `uint16_t` | Target relay channel (`0`..`3`) or element unicast address |
| +2 | `KND` | `uint8_t` | Action kind (`1` = Generic OnOff) |
| +3 | `VAL` | `uint8_t` | Desired state (`0` = OFF, `1` = ON) |

---

### 4.3. `SCHED_VND_OP_COMMIT` (Upload Finalize)
**Payload Length:** 5 bytes

```
Byte:   0     1     2     3     4
     +-----+-----+-----+-----+-----+
     | TID |       CRC32 (LE)      |
     +-----+-----+-----+-----+-----+
```

- `CRC32`: Standard IEEE 802.3 CRC32 calculated over all 60 preceding bytes of the `schedule_entry_t` structure.

---

### 4.4. `SCHED_VND_OP_STATUS` (Generic Reply)
**Payload Length:** 3 bytes

```
Byte:   0        1     2
     +--------+-----+-----+
     | STATUS | TID | ID  |
     +--------+-----+-----+
```

#### Status Codes:
| Code | Identifier | Description |
| :---: | :--- | :--- |
| `0x00` | `SCHED_STATUS_OK` | Operation successful |
| `0x01` | `SCHED_STATUS_ERR_BAD_VERSION` | Protocol version unsupported |
| `0x02` | `SCHED_STATUS_ERR_BAD_RANGE` | Invalid hour, minute, date, or repeat field |
| `0x03` | `SCHED_STATUS_ERR_TOO_MANY_ACTIONS` | Actions exceed `SCHED_MAX_ACTIONS` (8) |
| `0x04` | `SCHED_STATUS_ERR_STORAGE_FULL` | NVS storage space exhausted |
| `0x05` | `SCHED_STATUS_ERR_CRC_MISMATCH` | Computed CRC does not match commit frame |
| `0x06` | `SCHED_STATUS_ERR_BUSY` | Staging slots occupied (max 2 concurrent uploads) |
| `0x07` | `SCHED_STATUS_ERR_UNKNOWN_ID` | Requested schedule ID not found |
| `0x08` | `SCHED_STATUS_WARN_TIME_NOT_SYNCED` | Stored cleanly, but system time not yet NTP-synced |
| `0x09` | `SCHED_STATUS_ERR_DUPLICATE_TARGET` | Same schedule contains multiple actions for same relay |
| `0x0A` | `SCHED_STATUS_ERR_STAGING_TIMEOUT` | Staging buffer expired (> 15 s) |
| `0x0B` | `SCHED_STATUS_ERR_INVALID_PARAM` | Malformed message payload |

---

### 4.5. `SCHED_VND_OP_DELETE` & `SCHED_VND_OP_RUN_NOW`
**Payload Length:** 2 bytes
- Byte 0: `TID`
- Byte 1: `ID` (1..16)

### 4.6. `SCHED_VND_OP_ENABLE`
**Payload Length:** 3 bytes
- Byte 0: `TID`
- Byte 1: `ID` (1..16)
- Byte 2: `ENABLE` (`1` = Enable, `0` = Disable)

### 4.7. `SCHED_VND_OP_LIST` & `SCHED_VND_OP_CLEAR`
**Payload Length:** 1 byte
- Byte 0: `TID`

### 4.8. `SCHED_VND_OP_LIST_STATUS`
**Payload Length:** 4 bytes
- Byte 0: `TID`
- Byte 1: `COUNT` (Active count, 0..16)
- Bytes 2..3: `ACTIVE_MASK` (`uint16_t`, Little-Endian bitmask of active IDs, bit 0 = ID 1)

### 4.9. `SCHED_VND_OP_TIME_STATUS`
**Payload Length:** 11 bytes
- Byte 0: `TID`
- Byte 1: `TIME_VALID` (`1` if NTP calibrated, `0` if uncalibrated)
- Bytes 2..5: `LAST_SYNC_EPOCH` (`uint32_t`, UTC epoch timestamp of last sync)
- Bytes 6..7: `TZ_OFFSET_MIN` (`int16_t`, +330 for IST)
- Byte 8: `STORED_COUNT` (Total stored schedules in NVS)
- Byte 9: `FREE_SLOTS` (Remaining free capacity, max 16)
- Byte 10: `VERSION` (`1`)

---

## 5. Chunked Upload Sequence Diagram

```text
Flutter App                                   Master Node (Element 0)
    |                                                    |
    |---- 1. SCHED_BEGIN (TID=1, ID=1, 2 Actions) ------>| Allocates RAM Staging Buffer
    |<--- 2. SCHED_STATUS (OK, TID=1, ID=1) -------------| (15-second timeout window)
    |                                                    |
    |---- 3. SCHED_ACTIONS (TID=1, Seq=0, 2 Actions) --->| Staged actions appended
    |<--- 4. SCHED_STATUS (OK, TID=1, ID=1) -------------|
    |                                                    |
    |---- 5. SCHED_COMMIT (TID=1, CRC32) --------------->| Computes IEEE 802.3 CRC32
    |                                                    | Validates field ranges & targets
    |                                                    | Atomically writes to NVS Flash
    |                                                    | Frees Staging Buffer
    |<--- 6. SCHED_STATUS (OK / WARN_SYNC, TID=1) -------|
```

---

## 6. Execution & Deduplication Rules

1. **Local Relays:**
   - Targets are element unicast addresses (the app sends `lamp.elementAddress`). On a provisioned master, primary element + offset `0..3` resolves to local Relays `1..4` (same mapping as manual control).
   - Raw channels `0..3` are accepted only while the master is unprovisioned; once provisioned, `0x0001..0x0003` are real unicast addresses and are never treated as channel numbers.
   - Lamps on other nodes are sent a Generic OnOff Set Unacknowledged (3 sends, same TID) from the master's element-0 Generic OnOff Server.
   - Local relay commands are spaced 50 ms apart so the Holtek co-processor receives them one at a time, as with manual control.
2. **Conflict Deduplication (Last Action Wins):**
   - If multiple active schedules trigger at the exact same minute and target the same relay with conflicting states, the actions are merged and **the schedule with the highest ID executes last**.
3. **Auto-Disable for ONCE Schedules:**
   - Schedules configured with `SCHED_REPEAT_ONCE` automatically flip `enabled = 0` in NVS upon successful minute rollover execution.
4. **Time Reference:**
   - Evaluated strictly in IST (`UTC+5:30`). If NTP is uncalibrated at boot, the scheduler engine remains safely paused until the first successful SNTP sync.
