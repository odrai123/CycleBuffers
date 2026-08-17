# CycleBuffers

**Smaller machine buffers. Faster manifolds.**

CycleBuffers is a Satisfactory mod that limits production-machine and generator buffers by **production cycles** instead of normal item stack size.

This helps manifold-style factories ramp up faster by preventing machines near the start of a line from absorbing large quantities of material before downstream machines receive enough to begin production.

## Features

- Limits manufacturer input buffers by recipe cycles
- Limits manufacturer output buffers by recipe cycles
- Supports solid and fluid recipe inputs
- Supports solid and fluid generator fuels
- Supports supplemental generator water
- Separate configurable input, output, and generator cycle counts
- Generator buffer limiting can be enabled or disabled independently
- Settings are stored per save
- Settings can be changed live without restarting
- Recipe quantities respect session/world recipe-cost modifiers
- Compatible with increased item stack sizes
- Existing excess inventory is preserved and allowed to drain naturally
- Output buffers are never increased beyond the item's effective stack size
- Generator water buffers are never increased beyond the generator's native supplemental-fluid capacity
- Nuclear waste handling is untouched
- Designed to have very low runtime overhead

## How it works

CycleBuffers uses the actual amount of material consumed or produced by a recipe.

For example, if a recipe consumes:

```text
2 Iron Plates
3 Screws
```

and the input buffer is configured to 3 cycles, the machine will buffer approximately:

```text
6 Iron Plates
9 Screws
```

rather than filling to the normal stack size.

Generator buffers work in the same way.

For example, a Coal Generator uses:

```text
1 Coal
3 m³ Water
```

per fuel cycle.

With a generator buffer setting of 3 cycles, CycleBuffers will therefore target:

```text
3 Coal
9 m³ Water
```

Supplemental water is additionally capped at the generator's original native water capacity.

## Configuration

Settings are available per save from:

```text
Pause Menu
→ Mod Savegame Settings
→ Cycle Buffers
```

Default values:

```text
Input Buffer Cycles:       3
Output Buffer Cycles:     10

Enable Generator Buffers: On
Generator Buffer Cycles:   3
```

These are intended as sensible starting values and can be changed to suit your factory.

## Existing saves

CycleBuffers can be enabled on an existing save.

If a machine or generator already contains more material than its new buffer limit, CycleBuffers does **not** delete the excess.

The machine will simply stop accepting more material until the existing buffer falls below the configured limit.

Because of this, enabling CycleBuffers on a large established manifold may temporarily change where backpressure occurs while oversized buffers drain down.

## Compatibility

CycleBuffers has been tested with:

- Standard production machines
- Solid recipe inputs
- Fluid recipe inputs
- Solid-fuel generators
- Fluid-fuel generators
- Coal Generators and supplemental water
- Live setting changes
- Existing saves containing already-filled buffers
- Increased stack-size mods
- Stack Resizer

Nuclear generators use the same generator fuel and supplemental-water systems and are supported by the implementation, although nuclear-specific behaviour has not yet been tested in-game.

## Performance

CycleBuffers avoids per-item inventory hooks for production machines and keeps generator checks lightweight.

It is designed for large factories where manifold behaviour matters most.

## Licence

CycleBuffers is licensed under the GNU General Public License v3.0 or later.

See [LICENSE](LICENSE) for details.
