# Template data at runtime (static analysis, build 1269579)

How the engine hands an entity its **template** half of a component (the data every entity spawned from one template
shares, e.g. `ActiveNpcState`, `Enemy01BehaviorState`), and how EMF reads it.

## Functions

| Function | Address | What it does | Conf. |
|---|---|---|---|
| getTemplateComponent (world) | `0x1409a6f00` | `void*(world, u32 qualifiedHash, u32 entity)`: looks the hash up in the template map `*(world+0xE15DC0)+0x30` (= `world+0x958`), then tail-calls `0x1409a6e20`. EMF signature `GetTemplateComponent`. | High |
| getTemplateComponent (storage) | `0x1409a6ec0` | same with the entity storage (`world+0xE15DC0`) as first argument | High |
| template pointer of an entity | `0x1409a6e20` | `void*(storage, u16* typeIndex, u32 entity)`: entity record → archetype; if the archetype's bitset has the type, returns `*(chunk + archetype[0x584 + 2*type] + 8*indexInChunk)` | High |
| getComponent core | `0x1409a5ec0` | the dynamic counterpart: `{chunk + archetype[0x84 + 2*type] + archetype[0xa84 + 2*type]*index, stride}` | High |
| template-or-dynamic lookup | `0x1409a63a0` | tries the template map (`world+0x958`) first, then the dynamic map (`world+0x9b0`), then builds a write record (`0x1409b2a50`) | Medium |
| find entity by UniqueId | `0x1409a6420` | walks every archetype/chunk for the `UniqueId` template component; logs `Could not find Entity for UniqueId '%k'` | Medium |

## Layout (offsets from the entity storage `S = world+0xE15DC0`)

- `S+0x148` hash map EntityId → record index (`0x1409a8070`), `S+0x158` record count, `S+0x188` → record pointer array.
- Record: `+0x18` archetype, `+0x20` chunk data base, `+0x30` u32 index of the entity in its chunk.
- Archetype: `+0x00` component bitset (one bit per type index), `+0x84 + 2*t` u16 column offset of dynamic type `t`,
  `+0xa84 + 2*t` u16 stride of dynamic type `t`, `+0x584 + 2*t` u16 column offset of the per-entity **template
  pointer** for template type `t` (8 bytes per entity).
- Program iterators get the same pointers per entity: `J+0x470[i]` → per-slice array of template pointers
  (`iter_next` 0x1409b5a50 copies `*(J+0x470[i] + 8*entityIndex)` into the iterator's template slots).

## Size of a component (reflection)
- `storage[0]` (= `*(world+0xE15DC0)`) is the component registry (`world+0x928`). `registry+0x08` type entries*, `registry+0x10` u64 count (as
  `0x1409e6570` reads them), entries of 0x100 bytes indexed by the u16 type index from the template map (`registry+0x30`) or dynamic map (`+0x88`).
- Type entry (filled in `createWorld`'s builder `0x140974bf0`): `+0x00` name (ptr,len), `+0x20` hash, `+0x28` the
  descriptor used for chunk storage (the `DynamicX` half of a split component, or the component itself), `+0x30` the
  component's own reflection descriptor, `+0x38` u16 storage size. A reflection descriptor holds `+0x40` u32 size and
  `+0x50` u32 qualifiedHash (as for events), so EMF reads a template component's size as
  `*(u32*)(entry[+0x30] + 0x40)` after checking `entry[+0x30] + 0x50 == hash` (`WorldView::template_component_size`).

## What this means for mods
- The pointer is per entity but points at shared template data, so writing changes every entity of that template
  in that world. Whether the server and client worlds share the same data is checked in game (see ENGINE_MAP).
- Template data is read by game code whenever it needs it (for example `enemy01` reads `guardSetup` from the
  template each update), so a write takes effect without respawning. Values the game copies into a dynamic half at
  spawn keep their old value.
- Nothing is saved: the template comes from the game's resources again on the next start.
