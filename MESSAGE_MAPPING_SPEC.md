# Message Mapping Specification

## Data Sources (IL2CPP Reflection)

### 1. MsgId → Name Mapping
- **Class**: `Game.Message.MsgDef` (also `Game.Message.EnumMsgDef` as enum)
- **Pattern**: Static `Int32` fields where field name = message name, value = msgId
- **Naming convention**:
  - `CS*` = Client→Server (send)
  - `SC*` = Server→Client (recv)
  - No prefix = shared/notification
- **Count**: ~2042 message definitions
- **Examples**:
  - `CSLogin` = 1001
  - `SCLogin` = 1002
  - `SCFighterBagMsg` = 1003
  - `SCLoginFinish` = 1011
  - `CSEnterWord` = 1012
  - `SCEnterWord` = 1013
  - `CSPing` = 1502
  - `SCPong` = 1503

### 2. Message Field Schema
- **Namespace**: `Game.Message`
- **Pattern**: Each message name (e.g. `SCPong`) has a corresponding class `Game.Message.SCPong`
- **Parent**: All inherit from `MessageBase`
- **Protobuf library**: `Google.ProtocolBuffers` (protobuf-csharp-port, proto2 style)

### 3. Field Mapping Rules

#### Instance fields (the actual proto fields):
- **`hasXxx` (Boolean)**: Optional field presence marker → skip, not a proto field
- **`xxx_` (trailing underscore)**: The actual value field
- **`memoizedSerializedSize` (Int32)**: Internal cache → skip
- **Field order**: Fields appear in proto field number order (field 1, field 2, ...)
  - So the first non-skip field = proto field 1, second = field 2, etc.

#### Properties/methods to skip:
- `MessageID` — returns the msgId constant
- `IsInitialized` — proto2 required fields check
- `SerializedSize` / `CalcSerializedSize` — serialization cache
- `WriteTo` / `MergeFrom` — serialization methods
- `.ctor` — constructor

#### Type mapping (C# → protobuf wire type):
| C# Type | Proto Type | Wire Type |
|---------|-----------|-----------|
| `System.Int32` | int32 | varint (0) |
| `System.Int64` | int64 | varint (0) |
| `System.UInt32` | uint32 | varint (0) |
| `System.UInt64` | uint64 | varint (0) |
| `System.Boolean` | bool | varint (0) |
| `System.Single` | float | fixed32 (5) |
| `System.Double` | double | fixed64 (1) |
| `System.String` | string | length-delimited (2) |
| `System.Byte[]` | bytes | length-delimited (2) |
| `System.Collections.Generic.List<T>` | repeated T | length-delimited (2) |
| `Game.Message.XxxMsg` (class ref) | message Xxx | length-delimited (2) |

#### Example: SCPong (msgId 1503)
```
Fields:
  hasServerTime (Boolean) → optional marker, skip
  serverTime_ (Int64) → field 1: int64 serverTime
  memoizedSerializedSize → skip

Proto equivalent:
  message SCPong {
    optional int64 serverTime = 1;
  }
```

#### Example: CSLogin (msgId 1001)
```
Fields (in order, skipping has*/memoized):
  accid_ (String) → field 1: string accid
  playerId_ (String) → field 2: string playerId
  platform_ (String) → field 3: string platform
  token_ (String) → field 4: string token
  reconnect_ (Int32) → field 5: int32 reconnect
  sourceVersion_ (String) → field 6: string sourceVersion
  dataVersion_ (String) → field 7: string dataVersion
  language_ (String) → field 8: string language
  guideLogin_ (Boolean) → field 9: bool guideLogin
```

#### Example: SCFighterBagMsg (msgId 1003) — nested message
```
Fields:
  fighterBagMsg_ (Game.Message.FighterBagMsg) → field 1: FighterBagMsg

FighterBagMsg sub-fields:
  baseFighters_ (List<FighterMsg>) → field 1: repeated FighterMsg
  jobtrees_ (List<BaseJobTree>) → field 2: repeated BaseJobTree
  formations_ (List<FormationMsg>) → field 3: repeated FormationMsg
  ... etc
```

### 4. BattleOp / Operates Pattern

The `operates_` field appears in multiple battle-end messages and is a special binary blob
containing the player's battle actions. It is NOT standard protobuf — it's a custom binary
format that the JS `decBop()` function already handles.

#### How it appears in IL2CPP:
- **Type**: `Google.ProtocolBuffers.ByteString` (raw bytes, not a nested message)
- **Field name**: always `operates_` with `hasOperates` marker
- **Found in**: `BattleOperate`, `PveBattleEndData`, `CSNewOtoBattleEnd`,
  `CSExpeditionBattleResultReq`, `CSMazeNodeBattleEnd`, `CSDrawFairBattleAward`,
  `CSOtoBattleEnd`, `CSDrawFriendShipDungeonReward`, `CSTowerEvent`, etc.
- In `OnlineBattleFieldData`: `operates_` is `List<ByteString>` (repeated bytes)

#### BattleOperate message structure (proto):
```
message BattleOperate {
  optional bytes operates = 1;        // custom binary BattleOp blob
  optional PlayerFrameCheckResult checkResult = 2;
}
```

#### The binary BattleOp format (parsed by JS `decBop()`):
```
Entry[] — concatenated, no outer length
Each entry:
  u32le  blockLen     — length of the rest of this entry (after these 4 bytes)
  u32le  header       — entry header/type
  proto  fields       — protobuf fields within this entry:
    field 1 (varint): itm — item/action type
    field 2 (ld):     entity blob — nested proto with field 1 = entity info
    field 4 (varint): seq — sequence number
    field 5 (varint): tick — tick/timestamp (if n1/zigzag negative → empty marker)
```

#### JS integration (already works):
- `discoverBop()` auto-detects operates data inside any proto field by scoring `decBop()`
- It walks through nested proto fields recursively (up to depth 8) to find the bytes blob
- When `operates_` is a proto `bytes` field, the wire format is: varint tag + varint length + raw bytes
- The JS `doParse()` discovers it automatically — no special handling needed per-message
- The `bopPath` tracks which proto field numbers lead to the BattleOp bytes

#### Schema hint for JS:
When a field has type `Google.ProtocolBuffers.ByteString` and name contains "operates",
the schema should mark it with `"battleOp": true` so the UI can flag it.

### 5. API Design

#### `GET /api/msgdefs`
Returns the complete msgId→name mapping in one call.
```json
{
  "1001": "CSLogin",
  "1002": "SCLogin",
  "1003": "SCFighterBagMsg",
  ...
}
```
Implementation: iterate all static fields of `Game.Message.MsgDef`, read each Int32 value.

#### `GET /api/msgschema?name=SCPong`
Returns the proto field schema for a message class.
```json
{
  "name": "SCPong",
  "msgId": 1503,
  "fields": [
    { "num": 1, "name": "serverTime", "type": "int64", "optional": true }
  ]
}
```
Implementation:
1. Find class `Game.Message.<name>`
2. Iterate instance fields, skip `has*`, `memoized*`
3. For each value field `xxx_`: strip trailing `_`, check if matching `hasXxx` exists (→ optional)
4. Map C# type to proto type string
5. If type is `List<T>`, mark as `repeated` and resolve T
6. If type is a `Game.Message.*` class, mark as `message` and include sub-type name

### 5. Web UI Integration

#### Packet list:
- On WS connect, fetch `/api/msgdefs` → build `msgIdToName` lookup
- Show message name next to msgId in packet list rows

#### Detail panel:
- After `doParse()`, fetch `/api/msgschema?name=<name>`
- Overlay field names on parsed proto fields (field 1 = schema.fields[0].name, etc.)
- Show type annotations from schema

#### Packet crafter:
- Autocomplete/dropdown for message name → fills in msgId
- Show expected fields for the selected message type
